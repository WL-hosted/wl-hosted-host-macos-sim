#include "sim.h"

#include <string.h>

static void *executor_main(void *context) {
    sim_executor_t *executor = context;
    for (;;) {
        sim_task_t task;

        pthread_mutex_lock(&executor->mutex);
        while (executor->count == 0u && !executor->stopping)
            pthread_cond_wait(&executor->condition, &executor->mutex);
        if (executor->count == 0u && executor->stopping) {
            pthread_mutex_unlock(&executor->mutex);
            break;
        }

        task = executor->queue[executor->head];
        executor->head = (executor->head + 1u) % 64u;
        executor->count--;
        pthread_mutex_unlock(&executor->mutex);
        task.function(task.context);
    }
    return NULL;
}

int sim_executor_start(sim_executor_t *executor) {
    memset(executor, 0, sizeof(*executor));
    if (pthread_mutex_init(&executor->mutex, NULL) != 0)
        return -1;
    if (pthread_cond_init(&executor->condition, NULL) != 0)
        return -1;
    return pthread_create(&executor->thread, NULL, executor_main, executor);
}

void sim_executor_stop(sim_executor_t *executor) {
    pthread_mutex_lock(&executor->mutex);
    executor->stopping = true;
    pthread_cond_broadcast(&executor->condition);
    pthread_mutex_unlock(&executor->mutex);
    pthread_join(executor->thread, NULL);
    pthread_cond_destroy(&executor->condition);
    pthread_mutex_destroy(&executor->mutex);
}

int sim_executor_post(void *context, sim_task_fn function, void *task_context) {
    sim_executor_t *executor = context;
    size_t tail;
    pthread_mutex_lock(&executor->mutex);
    if (executor->stopping || executor->count == 64u) {
        pthread_mutex_unlock(&executor->mutex);
        return -1;
    }
    tail = (executor->head + executor->count) % 64u;
    executor->queue[tail] = (sim_task_t){function, task_context};
    executor->count++;
    pthread_cond_signal(&executor->condition);
    pthread_mutex_unlock(&executor->mutex);
    return 0;
}
