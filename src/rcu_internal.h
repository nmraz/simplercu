#pragma once

#include <stdalign.h>
#include <stdatomic.h>
#include <threads.h>

#define CACHE_LINE_SIZE 64
#define CACHE_ALIGNED alignas(CACHE_LINE_SIZE)

#ifdef TEST_STORE_BUFFERING
// When we're testing store buffering prevention, make sure `need_qs` resides in
// a separate cache line from `read_lock_nesting`.
#define NEW_CACHELINE_FOR_STORE_BUFFERING_TEST CACHE_ALIGNED
#else
#define NEW_CACHELINE_FOR_STORE_BUFFERING_TEST
#endif

#define unlikely(X) __builtin_expect(!!(X), 0)

struct rcu_thread_state {
    atomic_int read_lock_nesting;
    struct rcu_thread_state* next;
    struct rcu_thread_state** pprev;

    NEW_CACHELINE_FOR_STORE_BUFFERING_TEST atomic_bool need_qs;
};

extern thread_local CACHE_ALIGNED struct rcu_thread_state rcu_thread_state;

void rcu_read_unlock_report_qs(void);

static inline void asymm_fence_seq_cst_light(void) {
    atomic_signal_fence(memory_order_seq_cst);
}
