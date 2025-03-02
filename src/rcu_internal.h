#pragma once

#include <stdalign.h>
#include <stdatomic.h>
#include <threads.h>

#define CACHE_LINE_SIZE 64
#define CACHE_ALIGNED alignas(CACHE_LINE_SIZE)

#define unlikely(X) __builtin_expect(!!(X), 0)

struct rcu_thread_state {
    atomic_int read_lock_nesting;
    atomic_bool need_qs;
    struct rcu_thread_state* next;
    struct rcu_thread_state** pprev;
};

extern thread_local CACHE_ALIGNED struct rcu_thread_state rcu_thread_state;

void rcu_read_unlock_report_qs(void);

static inline void asymm_fence_seq_cst_light(void) {
    atomic_signal_fence(memory_order_seq_cst);
}
