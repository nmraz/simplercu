#include "rcu.h"

#include <linux/futex.h>
#include <linux/membarrier.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <threads.h>
#include <unistd.h>

struct rcu_state {
    mtx_t gp_lock;
    struct rcu_thread_state* thread_head;
    uint32_t thread_count;
    _Atomic(uint32_t) gp_holdouts;
};

static CACHE_ALIGNED struct rcu_state global_state;
thread_local CACHE_ALIGNED struct rcu_thread_state rcu_thread_state;

static void asymm_fence_seq_cst_heavy(void) {
    syscall(SYS_membarrier, MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0);
}

static void futex_wait(_Atomic(uint32_t)* ptr, uint32_t expected) {
    syscall(SYS_futex, FUTEX_WAIT, expected, NULL);
}

static void futex_wake(_Atomic(uint32_t)* ptr) {
    syscall(SYS_futex, FUTEX_WAKE, ptr);
}

void rcu_read_unlock_report_qs(void) {
    if (!atomic_exchange_explicit(&rcu_thread_state.need_qs, false,
                                  memory_order_relaxed)) {
        // The initiator has already noticed we were quiescent: it is
        // responsible for synchronizing with this reader, and will do so by
        // means of asymmetric fence G.
        return;
    }

    // We've cleared `need_qs` ourselves, so we're responsible for ensuring some
    // kind of synchronization with the end of the grace period.
    // `synchronize_rcu` issues an asymmetric fence (F) itself after setting
    // `need_qs`, but that fence might hit us too early (in the middle of the
    // critical section). Formally, F might precede lightweight barriers B and C
    // in the global SC order, while we want the end of the reader to precede
    // the end of the grace period.

    // Fence D: Synchronizes-with E and H.
    //
    // * Synchronization with E occurs via the RMW of `need_qs` above, and
    //   ensures we observe at least the initial state of `gp_holdouts` at the
    //   start of the grace period.
    //
    // * Synchronization with H occurs via the RMW to `gp_holdouts` (either
    //   because of a direct read or as part of a release sequence), and ensures
    //   that we happen-before the end of the grace period.
    atomic_thread_fence(memory_order_acq_rel);

    if (atomic_fetch_sub_explicit(&global_state.gp_holdouts, 1,
                                  memory_order_relaxed) == 1) {
        // We were the last holdout for this grace period, wake the initiator.
        futex_wake(&global_state.gp_holdouts);
    }
}

int rcu_init(void) {
    if (syscall(SYS_membarrier, MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED, 0,
                0) != 0) {
        return -1;
    }

    if (mtx_init(&global_state.gp_lock, mtx_plain) != thrd_success) {
        return -1;
    }

    return 0;
}

void rcu_thread_online(void) {
    mtx_lock(&global_state.gp_lock);
    rcu_thread_state.next = global_state.thread_head;
    rcu_thread_state.pprev = &global_state.thread_head;
    if (global_state.thread_head) {
        global_state.thread_head->pprev = &rcu_thread_state.next;
    }
    global_state.thread_head = &rcu_thread_state;
    global_state.thread_count++;
    mtx_unlock(&global_state.gp_lock);
}

void rcu_thread_offline(void) {
    mtx_lock(&global_state.gp_lock);
    *rcu_thread_state.pprev = rcu_thread_state.next;
    if (rcu_thread_state.next) {
        rcu_thread_state.next->pprev = rcu_thread_state.pprev;
    }
    global_state.thread_count--;
    mtx_unlock(&global_state.gp_lock);
}

void synchronize_rcu(void) {
    // clang-format off
    //
    // Every grace period G must ensure the following:
    //
    // 1. An SC fence is issued at some point during G.
    //
    // 2. For every reader R, at least one of the following holds:
    //
    //     i. The start of G happens-before the start of R after an SC fence has
    //        been issued on the thread performing G.
    //
    //    ii. The end of R happens-before the end of G, after which an SC fence
    //        is issued on the thread performing G.
    //
    // For most typical RCU applications, the happens-before relationships with
    // readers are sufficient.
    //
    // The SC fences start to matter when combining RCU external SC operations.
    // For example, the following store buffering scenario:
    //
    //     store_relaxed(&x, 1);  || store_relaxed(&y, 1);
    //     synchronize_rcu();     || fence_seq_cst();
    //     load_relaxed(&y); // 0 || load_relaxed(&x); // 0
    //
    // is forbidden by condition 1.
    //
    // Similarly, the more complex store buffering cycle here:
    //
    //     store_relaxed(&x, 1);  || rcu_read_lock();       || rcu_read_lock();
    //     synchronize_rcu();     || store_relaxed(&y, 1);  || store_relaxed(&z, 1);
    //     load_relaxed(&y); // 0 || load_relaxed(&z); // 0 || fence_seq_cst();
    //                            || rcu_read_unlock();     || load_relaxed(&x); // 0
    //                            ||                        || rcu_read_unlock();
    //
    // is prevented by the SC fence requirement in condition 2.i.
    //
    // Analogously, the SC fence requirement in condition 2.ii prevents this:
    //
    //     store_relaxed(&x, 1);  || rcu_read_lock();       || rcu_read_lock();
    //     synchronize_rcu();     || store_relaxed(&y, 1);  || store_relaxed(&z, 1);
    //     load_relaxed(&y); // 0 || fence_seq_cst();       || load_relaxed(&x); // 0
    //                            || load_relaxed(&z); // 0 || rcu_read_unlock();
    //                            || rcu_read_unlock();     ||
    //
    // clang-format on

    uint32_t thread_count = 0;
    uint32_t quiescent = 0;

    mtx_lock(&global_state.gp_lock);

    thread_count = global_state.thread_count;

    atomic_store_explicit(&global_state.gp_holdouts, thread_count,
                          memory_order_relaxed);

    // Fence E: Synchronizes-with D via the writes to `need_qs` below.
    // Ensures that readers reporting themselves as quiescent observe a
    // `gp_holdouts` from the current grace period.
    atomic_thread_fence(memory_order_release);

    for (struct rcu_thread_state* thread = global_state.thread_head; thread;
         thread = thread->next) {
        atomic_store_explicit(&thread->need_qs, true, memory_order_relaxed);
    }

    // Fence F: Pairs with A and C, upholds requirements 1 and 2.i above.
    //
    // * The pairing with A ensures that if our read of `read_lock_nesting`
    //   reads-before a particular `rcu_read_lock`, everything preceding the
    //   grace period will happen-before that `rcu_read_lock`.
    //
    // * The pairing with C prevents store buffering and makes sure that for
    //   every top-level read-side critical section exited, either we'll observe
    //   the store to `read_lock_nesting` preceding C in the loop below, or that
    //   `rcu_read_unlock` will observe our store to `need_qs` in the loop
    //   above.
    //
    // * The SC fence performed on the current thread by this function upholds
    //   requirement 1 for this grace period, as well as the SC-fence portion
    //   of 2.i for any readers it happens-before.
    asymm_fence_seq_cst_heavy();

    for (struct rcu_thread_state* thread = global_state.thread_head; thread;
         thread = thread->next) {
        if (atomic_load_explicit(&thread->read_lock_nesting,
                                 memory_order_relaxed) == 0) {
            if (atomic_exchange_explicit(&thread->need_qs, false,
                                         memory_order_relaxed)) {
                // This thread is quiescent, and we now claim responsibility for
                // reporting that (see fence G below for memory ordering
                // guarantees).
                quiescent++;
            }
        }
    }

    if (quiescent > 0) {
        // Self-report any threads we've noticed are quiescent.

        // Fence G: Pairs with B, upholds the SC-fence portion of
        // requirement 2.ii above.
        //
        // * The pairing with B ensures that if we observe a `read_lock_nesting`
        //   of 0 above and manage to claim responsibility for marking a given
        //   thread as quiescent, we will also observe any accesses inside the
        //   read-side critical section.
        //
        // * The SC fence performed on the current thread before the function
        //   returns upholds the SC-fence portion of requirement 2.ii for any
        //   readers with which it has synchronized.

        asymm_fence_seq_cst_heavy();
        atomic_fetch_sub_explicit(&global_state.gp_holdouts, quiescent,
                                  memory_order_relaxed);
    }

    if (quiescent != thread_count) {
        // If we haven't reported all online threads as quiescent ourselves, we
        // need to wait until the last one reports itself via
        // `rcu_read_unlock_report_qs` and then perform an SC fence.

        for (;;) {
            uint32_t holdouts = atomic_load_explicit(&global_state.gp_holdouts,
                                                     memory_order_relaxed);
            if (holdouts == 0) {
                break;
            }
            futex_wait(&global_state.gp_holdouts, holdouts);
        }

        // Fence H: Synchronizes-with with D, ensures we perform an SC fence
        // as per requirement 2.ii above.
        //
        // * The synchronization with D that happens via our read of
        //   `gp_holdouts` and ensures the ends of the readers we were waiting
        //   for happen-before the end of the grace period.
        //
        // * The fence is an SC one (as opposed to acquire) to uphold
        //   rquirement 2.ii.
        atomic_thread_fence(memory_order_seq_cst);
    }

    mtx_unlock(&global_state.gp_lock);
}
