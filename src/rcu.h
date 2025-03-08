#pragma once

#include <stdatomic.h>

#include "rcu_internal.h"

int rcu_init(void);

void rcu_thread_online(void);
void rcu_thread_offline(void);

void synchronize_rcu(void);

static inline void rcu_read_lock(void) {
    int nesting = atomic_load_explicit(&rcu_thread_state.read_lock_nesting,
                                       memory_order_relaxed);
    atomic_store_explicit(&rcu_thread_state.read_lock_nesting, nesting + 1,
                          memory_order_relaxed);

    // Fence A: Pairs with F in `synchronize_rcu`.
    // Ensures that whenever a grace period initiator's read of this thread's
    // `read_lock_nesting` reads-before the increment above, accesses before
    // that grace will happen-before this read-side critical section.
    asymm_fence_seq_cst_light();
}

static inline void rcu_read_unlock(void) {
    // Fence B: Pairs with G in `synchronize_rcu`.
    // Ensures that if the initiator observes a store of 0 to
    // `read_lock_nesting` below and claims responsibility for reporting this
    // thread as quiescent, all accesses in the preceding read-side critical
    // section will happen-before the end of the grace period.
    asymm_fence_seq_cst_light();

    int nesting = atomic_load_explicit(&rcu_thread_state.read_lock_nesting,
                                       memory_order_relaxed);
    atomic_store_explicit(&rcu_thread_state.read_lock_nesting, nesting - 1,
                          memory_order_relaxed);

    if (nesting == 1) {

        // Fence C: Pairs with F in `synchronize_rcu`.
        // Prevents store buffering and ensures that either the initiator will
        // observe that we have become quiescent, or we will observe the
        // initiator's write to `need_qs`.

        asymm_fence_seq_cst_light();
        if (unlikely(atomic_load_explicit(&rcu_thread_state.need_qs,
                                          memory_order_relaxed))) {
            rcu_read_unlock_report_qs();
        }
    }
}
