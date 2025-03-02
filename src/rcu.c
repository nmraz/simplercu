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
        // responsible for executing the memory barrier on this thread, and will
        // do so by means of asymmetric barrier F.
        return;
    }

    // We've cleared `need_qs` ourselves, so we're responsible for issuing a
    // full memory barrier on the owning thread (us!). `synchronize_rcu` issues
    // an asymmetric barrier (E) itself after setting `need_qs`, but that
    // barrier might hit us too early (in the middle of the critical section).
    // Formally, E might precede lightweight barriers B and C in the global
    // SC order, while we want something that precedes the end of the grace
    // period in that order.

    // This barrier also synchronizes-with the release store to `need_qs` and
    // ensures we observe at least the initial state of `gp_holdouts` at the
    // start of the grace period.

    // Barrier D: Pairs with G in `synchronize_rcu`, synchronizes-with R in
    // `synchronize_rcu`.
    atomic_thread_fence(memory_order_seq_cst);

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
    uint32_t quiescent = 0;

    mtx_lock(&global_state.gp_lock);

    atomic_store_explicit(&global_state.gp_holdouts, global_state.thread_count,
                          memory_order_relaxed);

    // Barrier R: Synchronizes-with D.
    // Ensures that readers reporting themselves as quiescent observe a
    // `gp_holdouts` from the current grace period.
    atomic_thread_fence(memory_order_release);

    for (struct rcu_thread_state* thread = global_state.thread_head; thread;
         thread = thread->next) {
        atomic_store_explicit(&thread->need_qs, true, memory_order_relaxed);
    }

    // Barrier E: Pairs with A and C.
    //
    // * The pairing with A ensures that if our read of `read_lock_nesting`
    //   reads-before a particular `rcu_read_lock`, that `rcu_read_lock` will
    //   observe all accesses preceding the grace period.
    //
    // * The pairing with C prevents store buffering and makes sure that for
    //   every top-level read-side critical section exited, either we'll observe
    //   the store to `read_lock_nesting` preceding C in the loop below, or that
    //   `rcu_read_unlock` will observe our store to `need_qs` in the loop
    //   above.
    asymm_fence_seq_cst_heavy();

    for (struct rcu_thread_state* thread = global_state.thread_head; thread;
         thread = thread->next) {
        if (atomic_load_explicit(&thread->read_lock_nesting,
                                 memory_order_relaxed) == 0) {
            if (atomic_exchange_explicit(&thread->need_qs, false,
                                         memory_order_relaxed)) {
                // This thread is quiescent, and we now claim responsibility for
                // reporting that (see barrier F below for memory ordering
                // guarantees).
                quiescent++;
            }
        }
    }

    if (quiescent > 0) {
        // Self-report any threads we've noticed are quiescent.

        // Barrier F: Pairs with B.
        // Ensures that if we observe a `read_lock_nesting` of 0 above and
        // manage to claim responsibility for marking a given thread as
        // quiescent, we will also observe any accesses inside the read-side
        // critical section.

        asymm_fence_seq_cst_heavy();
        atomic_fetch_sub_explicit(&global_state.gp_holdouts, quiescent,
                                  memory_order_relaxed);
    }

    if (atomic_load_explicit(&global_state.gp_holdouts, memory_order_relaxed) >
        0) {
        for (;;) {
            uint32_t holdouts = atomic_load_explicit(&global_state.gp_holdouts,
                                                     memory_order_relaxed);
            if (holdouts == 0) {
                break;
            }
            futex_wait(&global_state.gp_holdouts, holdouts);
        }

        // Barrier G: Pairs with D in `rcu_read_unlock_report_qs`.
        atomic_thread_fence(memory_order_seq_cst);
    }

    mtx_unlock(&global_state.gp_lock);
}
