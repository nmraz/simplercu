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

unsigned update_interval_us;
unsigned test_time_ms;

int worker_func(void* arg) {
    uint64_t value;
    int i = (int) (intptr_t) arg;

    rcu_thread_online();

    while (!atomic_load_explicit(&should_exit, memory_order_relaxed)) {
        rcu_read_lock();

        // NOTE: This is actually a consume load, and we just "know" the
        // compiler won't break the dependency to the immediate load here.
        value =
            *atomic_load_explicit(&global_shared_state, memory_order_relaxed);

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

void update_global_state(void) {
    uint64_t* old_state = NULL;

    uint64_t* new_state = malloc(sizeof(*new_state));
    *new_state = 5;

    old_state = atomic_exchange_explicit(&global_shared_state, new_state,
                                         memory_order_release);

    if (old_state) {
        synchronize_rcu();
        *old_state = UINT64_MAX;
    }
}

int updater_func(void* arg) {
    while (!atomic_load_explicit(&should_exit, memory_order_relaxed)) {
        update_global_state();
        usleep(update_interval_us);
    }
    return 0;
}

bool parse_opts(int argc, char* argv[]) {
    int opt;

    while ((opt = getopt(argc, argv, "t:i:")) != -1) {
        switch (opt) {
        case 't':
            test_time_ms = atoi(optarg);
            break;
        case 'i':
            update_interval_us = atoi(optarg);
            break;
        default:
            return false;
        }
    }

    if (test_time_ms == 0) {
        fprintf(stderr, "%s: test duration must be specified\n", argv[0]);
        return false;
    }

    if (update_interval_us == 0) {
        fprintf(stderr, "%s: udpate interval must be specified\n", argv[0]);
        return false;
    }

    return true;
}

int main(int argc, char* argv[]) {
    thrd_t workers[WORKER_COUNT];
    thrd_t updater;

    if (!parse_opts(argc, argv)) {
        fprintf(stderr, "usage: %s -t <test_time_ms> -i <update_interval_us>\n",
                argv[0]);
        return 1;
    }

    if (rcu_init() != 0) {
        return 1;
    }

    rcu_thread_online();

    update_global_state();

    printf(
        "starting stresstest with %d workers for %ums, update interval %dμs\n",
        WORKER_COUNT, test_time_ms, update_interval_us);

    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (size_t i = 0; i < WORKER_COUNT; i++) {
        if (thrd_create(&workers[i], worker_func, (void*) (intptr_t) i) !=
            thrd_success) {
            return 1;
        }
    }

    if (thrd_create(&updater, updater_func, NULL) != thrd_success) {
        return 1;
    }

    usleep(test_time_ms * 1000);

    atomic_store_explicit(&should_exit, true, memory_order_relaxed);

    for (size_t i = 0; i < WORKER_COUNT; i++) {
        thrd_join(workers[i], NULL);
    }

    thrd_join(updater, NULL);

    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);

    uint64_t elapsed_us = (end.tv_sec - start.tv_sec) * 1000000 +
                          (end.tv_nsec - start.tv_nsec) / 1000;

    printf("stresstest complete in %lu.%02lus\n", elapsed_us / 1000000,
           (elapsed_us % 1000000) / 10000);

    rcu_thread_offline();

    return 0;
}
