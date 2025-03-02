#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/user.h>
#include <threads.h>
#include <unistd.h>

#include "rcu.h"

#define WORKER_COUNT 15

struct shared_state {
    uint64_t a;
    uint64_t b;
};

atomic_bool should_exit;
_Atomic(struct shared_state*) global_shared_state;

thread_local uint64_t final_sum;
thread_local uint64_t iterations;

int worker_func(void* arg) {
    struct shared_state* state;
    int i = (int) (intptr_t) arg;

    rcu_thread_online();

    while (!atomic_load_explicit(&should_exit, memory_order_relaxed)) {
        rcu_read_lock();

        // NOTE: This is actually a consume load, and we just "know" the
        // compiler won't break the dependency here.
        state =
            atomic_load_explicit(&global_shared_state, memory_order_relaxed);
        final_sum += state->a * state->b;
        iterations++;

        rcu_read_unlock();
    }

    printf("thread %d: sum %lu, iterations %lu\n", i, final_sum, iterations);

    rcu_thread_offline();

    return 0;
}

void update_global_state(uint64_t a, uint64_t b) {
    struct shared_state* old_state =
        atomic_load_explicit(&global_shared_state, memory_order_relaxed);

    struct shared_state* new_state =
        mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE,
             MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    new_state->a = a;
    new_state->b = b;

    atomic_store_explicit(&global_shared_state, new_state,
                          memory_order_release);

    if (old_state) {
        synchronize_rcu();
        munmap(old_state, PAGE_SIZE);
    }
}

int main(void) {
    thrd_t workers[WORKER_COUNT];

    if (rcu_init() != 0) {
        return 1;
    }

    rcu_thread_online();

    update_global_state(1, 2);

    for (size_t i = 0; i < WORKER_COUNT; i++) {
        if (thrd_create(&workers[i], worker_func, (void*) (intptr_t) i) !=
            thrd_success) {
            return 1;
        }
    }

    for (size_t i = 0; i < 10; i++) {
        printf("upd %zu\n", i);
        update_global_state(i + 2, (i + 2) * 2);
        usleep(500000);
    }

    atomic_store_explicit(&should_exit, true, memory_order_relaxed);

    for (size_t i = 0; i < WORKER_COUNT; i++) {
        thrd_join(workers[i], NULL);
    }

    rcu_thread_offline();

    return 0;
}
