#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/user.h>
#include <threads.h>
#include <time.h>
#include <unistd.h>

#include "rcu.h"

atomic_bool should_exit;
_Atomic(uint64_t*) global_shared_state;

thread_local uint64_t iterations;

int worker_func(void* arg) {
    uint64_t* state;
    uint64_t value;
    int i = (int) (intptr_t) arg;

    rcu_thread_online();

    while (!atomic_load_explicit(&should_exit, memory_order_relaxed)) {
        rcu_read_lock();

        // NOTE: This is actually a consume load, and we just "know" the
        // compiler won't break the dependency here.
        state =
            atomic_load_explicit(&global_shared_state, memory_order_relaxed);
        value = *state;

        if (value == UINT64_MAX) {
            abort();
        }

        iterations++;

        rcu_read_unlock();
    }

    printf("thread %d: %lu iterations\n", i, iterations);

    rcu_thread_offline();

    return 0;
}

void update_global_state(uint64_t n) {
    uint64_t* old_state =
        atomic_load_explicit(&global_shared_state, memory_order_relaxed);

    uint64_t* new_state = malloc(sizeof(*new_state));
    *new_state = n;

    atomic_store_explicit(&global_shared_state, new_state,
                          memory_order_release);

    if (old_state) {
        synchronize_rcu();
        *old_state = UINT64_MAX;
    }
}

int main(void) {
    thrd_t workers[WORKER_COUNT];

    if (rcu_init() != 0) {
        return 1;
    }

    rcu_thread_online();

    update_global_state(1);

    printf("starting stresstest with %d workers, update interval %dμs\n",
           WORKER_COUNT, UPDATE_INTERVAL_US);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (size_t i = 0; i < WORKER_COUNT; i++) {
        if (thrd_create(&workers[i], worker_func, (void*) (intptr_t) i) !=
            thrd_success) {
            return 1;
        }
    }

    for (size_t i = 0; i < 100000; i++) {
        update_global_state(i + 1);
        usleep(UPDATE_INTERVAL_US);
    }

    atomic_store_explicit(&should_exit, true, memory_order_relaxed);

    for (size_t i = 0; i < WORKER_COUNT; i++) {
        thrd_join(workers[i], NULL);
    }

    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);

    uint64_t elapsed_us = (end.tv_sec - start.tv_sec) * 1000000 +
                          (end.tv_nsec - start.tv_nsec) / 1000;

    printf("stresstest complete in %lu.%02lus\n", elapsed_us / 1000000,
           (elapsed_us % 1000000) / 10000);

    rcu_thread_offline();

    return 0;
}
