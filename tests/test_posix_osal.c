#include "posix_osal.h"

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

typedef struct callback_context {
    wlh_osal_ops_t *ops;
    wlh_osal_semaphore_t *semaphore;
    atomic_uint calls;
} callback_context_t;

static void signal_callback(void *argument) {
    callback_context_t *context = argument;
    atomic_fetch_add(&context->calls, 1u);
    assert(context->ops->semaphore_give(
               context->ops->context, context->semaphore
           ) == 0);
}

static void wait_callback(void *argument) {
    callback_context_t *context = argument;
    assert(context->ops->semaphore_take(
               context->ops->context, context->semaphore,
               WLH_OSAL_WAIT_FOREVER
           ) == 0);
}

int main(void) {
    wlh_posix_osal_t posix;
    wlh_osal_ops_t ops;
    wlh_osal_task_t task;
    wlh_osal_mutex_t mutex;
    wlh_osal_semaphore_t semaphore;
    wlh_osal_event_t event;
    wlh_osal_queue_t queue;
    wlh_osal_timer_t timer;
    callback_context_t callback;
    uint32_t queue_storage[2] = {0u, 0u};
    uint32_t value = 7u;
    uint32_t received = 0u;
    uint32_t observed = 0u;
    bool woken = false;

    wlh_posix_osal_init(&posix);
    ops = wlh_posix_osal_ops(&posix);
    assert(wlh_osal_ops_valid(&ops));
    assert(!ops.in_isr(ops.context));

    assert(ops.mutex_create(ops.context, &mutex) == 0);
    assert(ops.mutex_lock(ops.context, &mutex, 100u) == 0);
    assert(ops.mutex_lock(ops.context, &mutex, 10u) != 0);
    ops.mutex_unlock(ops.context, &mutex);
    ops.mutex_destroy(ops.context, &mutex);

    assert(ops.queue_create(
               ops.context, &queue, queue_storage, sizeof(uint32_t), 2u
           ) == 0);
    assert(ops.queue_send(ops.context, &queue, &value, 100u) == 0);
    assert(ops.queue_receive(ops.context, &queue, &received, 100u) == 0);
    assert(received == value);
    assert(ops.queue_receive(
               ops.context, &queue, &received, WLH_OSAL_NO_WAIT
           ) != 0);
    assert(ops.queue_send_from_isr(
               ops.context, &queue, &value, &woken
           ) == 0 && woken);
    assert(ops.queue_receive(ops.context, &queue, &received, 100u) == 0);
    ops.queue_destroy(ops.context, &queue);

    assert(ops.semaphore_create(ops.context, &semaphore, 0u, 2u) == 0);
    assert(ops.event_create(ops.context, &event) == 0);
    assert(ops.event_set_from_isr(ops.context, &event, 0x2u, &woken) == 0);
    assert(ops.event_wait(
               ops.context, &event, 0x2u, true, true, 100u, &observed
           ) == 0);
    assert(observed == 0x2u);

    callback.ops = &ops;
    callback.semaphore = &semaphore;
    atomic_init(&callback.calls, 0u);
    assert(ops.task_create(
               ops.context, &task, NULL, signal_callback, &callback
           ) == 0);
    assert(ops.semaphore_take(ops.context, &semaphore, 500u) == 0);
    assert(ops.task_join(ops.context, &task, 500u) == 0);

    assert(ops.task_create(
               ops.context, &task, NULL, wait_callback, &callback
           ) == 0);
    assert(ops.task_join(ops.context, &task, 10u) != 0);
    assert(ops.semaphore_give(ops.context, &semaphore) == 0);
    assert(ops.task_join(ops.context, &task, 500u) == 0);

    assert(ops.timer_create(
               ops.context, &timer, signal_callback, &callback
           ) == 0);
    assert(ops.timer_start(ops.context, &timer, 10u, false) == 0);
    assert(ops.semaphore_take(ops.context, &semaphore, 500u) == 0);
    assert(atomic_load(&callback.calls) == 2u);
    assert(ops.timer_stop(ops.context, &timer) == 0);
    ops.timer_destroy(ops.context, &timer);

    ops.event_destroy(ops.context, &event);
    ops.semaphore_destroy(ops.context, &semaphore);
    puts("POSIX OSAL consistency tests passed");
    return 0;
}
