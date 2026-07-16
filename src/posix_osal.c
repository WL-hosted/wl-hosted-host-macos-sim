#include "posix_osal.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

typedef struct task_state {
    pthread_t thread;
    wlh_osal_task_fn entry;
    void *argument;
    atomic_bool done;
    bool created;
} task_state_t;

typedef struct semaphore_state {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    uint32_t count;
    uint32_t maximum;
} semaphore_state_t;

typedef struct event_state {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    uint32_t bits;
} event_state_t;

typedef struct queue_state {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    uint8_t *storage;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t count;
} queue_state_t;

typedef struct timer_state {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    wlh_osal_timer_fn callback;
    void *argument;
    uint64_t deadline_ms;
    uint32_t period_ms;
    bool periodic;
    bool armed;
    bool stopping;
    bool created;
} timer_state_t;

_Static_assert(sizeof(task_state_t) <= sizeof(wlh_osal_task_t), "task opaque storage");
_Static_assert(sizeof(pthread_mutex_t) <= sizeof(wlh_osal_mutex_t), "mutex opaque storage");
_Static_assert(sizeof(semaphore_state_t) <= sizeof(wlh_osal_semaphore_t), "semaphore opaque storage");
_Static_assert(sizeof(event_state_t) <= sizeof(wlh_osal_event_t), "event opaque storage");
_Static_assert(sizeof(queue_state_t) <= sizeof(wlh_osal_queue_t), "queue opaque storage");
_Static_assert(sizeof(timer_state_t) <= sizeof(wlh_osal_timer_t), "timer opaque storage");

static uint64_t clock_ms(void) {
    struct timespec value;
    (void)clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

static struct timespec realtime_after(uint32_t timeout_ms) {
    struct timespec value;
    (void)clock_gettime(CLOCK_REALTIME, &value);
    value.tv_sec += (time_t)(timeout_ms / 1000u);
    value.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
    if (value.tv_nsec >= 1000000000L) {
        value.tv_sec++;
        value.tv_nsec -= 1000000000L;
    }
    return value;
}

static void sleep_duration(uint32_t duration_ms) {
    struct timespec request = {(time_t)(duration_ms / 1000u),
                               (long)(duration_ms % 1000u) * 1000000L};
    while (nanosleep(&request, &request) != 0 && errno == EINTR) {}
}

static int wait_condition(pthread_cond_t *condition, pthread_mutex_t *mutex,
                          uint32_t timeout_ms) {
    if (timeout_ms == WLH_OSAL_WAIT_FOREVER)
        return pthread_cond_wait(condition, mutex) == 0 ? 0 : -1;
    if (timeout_ms == WLH_OSAL_NO_WAIT)
        return -1;
    struct timespec deadline = realtime_after(timeout_ms);
    return pthread_cond_timedwait(condition, mutex, &deadline) == 0 ? 0 : -1;
}

static void *task_trampoline(void *argument) {
    task_state_t *state = argument;
    state->entry(state->argument);
    atomic_store(&state->done, true);
    return NULL;
}

static int os_task_create(void *context, wlh_osal_task_t *task,
                          const wlh_osal_task_attributes_t *attributes,
                          wlh_osal_task_fn entry, void *argument) {
    task_state_t *state = (task_state_t *)task;
    pthread_attr_t native_attributes;
    int result;
    (void)context;
    if (task == NULL || entry == NULL) return -1;
    memset(task, 0, sizeof(*task));
    state->entry = entry;
    state->argument = argument;
    atomic_init(&state->done, false);
    if (pthread_attr_init(&native_attributes) != 0) return -1;
    if (attributes != NULL && attributes->stack_size != 0u &&
        pthread_attr_setstacksize(&native_attributes, attributes->stack_size) != 0) {
        pthread_attr_destroy(&native_attributes);
        return -1;
    }
    result = pthread_create(&state->thread, &native_attributes, task_trampoline, state);
    pthread_attr_destroy(&native_attributes);
    state->created = result == 0;
    return result == 0 ? 0 : -1;
}

static int os_task_join(void *context, wlh_osal_task_t *task, uint32_t timeout_ms) {
    task_state_t *state = (task_state_t *)task;
    uint64_t deadline;
    (void)context;
    if (state == NULL || !state->created) return -1;
    if (timeout_ms == WLH_OSAL_WAIT_FOREVER) {
        if (pthread_join(state->thread, NULL) != 0) return -1;
    } else {
        deadline = clock_ms() + timeout_ms;
        while (!atomic_load(&state->done)) {
            if (timeout_ms == WLH_OSAL_NO_WAIT || clock_ms() >= deadline) return -1;
            sleep_duration(1u);
        }
        if (pthread_join(state->thread, NULL) != 0) return -1;
    }
    state->created = false;
    return 0;
}

static int os_mutex_create(void *context, wlh_osal_mutex_t *mutex) {
    (void)context;
    if (mutex == NULL) return -1;
    memset(mutex, 0, sizeof(*mutex));
    return pthread_mutex_init((pthread_mutex_t *)mutex, NULL) == 0 ? 0 : -1;
}
static void os_mutex_destroy(void *context, wlh_osal_mutex_t *mutex) {
    (void)context;
    if (mutex != NULL) (void)pthread_mutex_destroy((pthread_mutex_t *)mutex);
}
static int os_mutex_lock(void *context, wlh_osal_mutex_t *mutex, uint32_t timeout_ms) {
    uint64_t deadline;
    int result;
    (void)context;
    if (mutex == NULL) return -1;
    if (timeout_ms == WLH_OSAL_WAIT_FOREVER)
        return pthread_mutex_lock((pthread_mutex_t *)mutex) == 0 ? 0 : -1;
    deadline = clock_ms() + timeout_ms;
    do {
        result = pthread_mutex_trylock((pthread_mutex_t *)mutex);
        if (result == 0) return 0;
        if (result != EBUSY || timeout_ms == WLH_OSAL_NO_WAIT) return -1;
        sleep_duration(1u);
    } while (clock_ms() < deadline);
    return -1;
}
static void os_mutex_unlock(void *context, wlh_osal_mutex_t *mutex) {
    (void)context;
    if (mutex != NULL) (void)pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

static int os_semaphore_create(void *context, wlh_osal_semaphore_t *semaphore,
                               uint32_t initial_count, uint32_t maximum_count) {
    semaphore_state_t *state = (semaphore_state_t *)semaphore;
    (void)context;
    if (state == NULL || maximum_count == 0u || initial_count > maximum_count) return -1;
    memset(semaphore, 0, sizeof(*semaphore));
    if (pthread_mutex_init(&state->mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&state->condition, NULL) != 0) {
        pthread_mutex_destroy(&state->mutex);
        return -1;
    }
    state->count = initial_count;
    state->maximum = maximum_count;
    return 0;
}
static void os_semaphore_destroy(void *context, wlh_osal_semaphore_t *semaphore) {
    semaphore_state_t *state = (semaphore_state_t *)semaphore;
    (void)context;
    if (state == NULL) return;
    (void)pthread_cond_destroy(&state->condition);
    (void)pthread_mutex_destroy(&state->mutex);
}
static int os_semaphore_take(void *context, wlh_osal_semaphore_t *semaphore,
                             uint32_t timeout_ms) {
    semaphore_state_t *state = (semaphore_state_t *)semaphore;
    int result = 0;
    (void)context;
    if (state == NULL || pthread_mutex_lock(&state->mutex) != 0) return -1;
    while (state->count == 0u) {
        if (wait_condition(&state->condition, &state->mutex, timeout_ms) != 0) {
            result = -1;
            break;
        }
    }
    if (result == 0) state->count--;
    pthread_mutex_unlock(&state->mutex);
    return result;
}
static int semaphore_give_common(semaphore_state_t *state) {
    int result = 0;
    if (state == NULL || pthread_mutex_lock(&state->mutex) != 0) return -1;
    if (state->count == state->maximum) result = -1;
    else {
        state->count++;
        pthread_cond_signal(&state->condition);
    }
    pthread_mutex_unlock(&state->mutex);
    return result;
}
static int os_semaphore_give(void *context, wlh_osal_semaphore_t *semaphore) {
    (void)context;
    return semaphore_give_common((semaphore_state_t *)semaphore);
}
static int os_semaphore_give_from_isr(void *context,
                                      wlh_osal_semaphore_t *semaphore,
                                      bool *higher_priority_task_woken) {
    if (higher_priority_task_woken != NULL) *higher_priority_task_woken = true;
    return os_semaphore_give(context, semaphore);
}

static int os_event_create(void *context, wlh_osal_event_t *event) {
    event_state_t *state = (event_state_t *)event;
    (void)context;
    if (state == NULL) return -1;
    memset(event, 0, sizeof(*event));
    if (pthread_mutex_init(&state->mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&state->condition, NULL) != 0) {
        pthread_mutex_destroy(&state->mutex);
        return -1;
    }
    return 0;
}
static void os_event_destroy(void *context, wlh_osal_event_t *event) {
    event_state_t *state = (event_state_t *)event;
    (void)context;
    if (state == NULL) return;
    (void)pthread_cond_destroy(&state->condition);
    (void)pthread_mutex_destroy(&state->mutex);
}
static bool event_matches(uint32_t value, uint32_t bits, bool wait_all) {
    return wait_all ? (value & bits) == bits : (value & bits) != 0u;
}
static int os_event_wait(void *context, wlh_osal_event_t *event, uint32_t bits,
                         bool wait_all, bool clear_on_exit, uint32_t timeout_ms,
                         uint32_t *observed_bits) {
    event_state_t *state = (event_state_t *)event;
    int result = 0;
    (void)context;
    if (state == NULL || bits == 0u || observed_bits == NULL ||
        pthread_mutex_lock(&state->mutex) != 0) return -1;
    while (!event_matches(state->bits, bits, wait_all)) {
        if (wait_condition(&state->condition, &state->mutex, timeout_ms) != 0) {
            result = -1;
            break;
        }
    }
    if (result == 0) {
        *observed_bits = state->bits & bits;
        if (clear_on_exit) state->bits &= ~bits;
    }
    pthread_mutex_unlock(&state->mutex);
    return result;
}
static int os_event_set(void *context, wlh_osal_event_t *event, uint32_t bits) {
    event_state_t *state = (event_state_t *)event;
    (void)context;
    if (state == NULL || pthread_mutex_lock(&state->mutex) != 0) return -1;
    state->bits |= bits;
    pthread_cond_broadcast(&state->condition);
    pthread_mutex_unlock(&state->mutex);
    return 0;
}
static int os_event_set_from_isr(void *context, wlh_osal_event_t *event,
                                 uint32_t bits,
                                 bool *higher_priority_task_woken) {
    if (higher_priority_task_woken != NULL) *higher_priority_task_woken = true;
    return os_event_set(context, event, bits);
}

static int os_queue_create(void *context, wlh_osal_queue_t *queue, void *storage,
                           size_t item_size, size_t capacity) {
    queue_state_t *state = (queue_state_t *)queue;
    (void)context;
    if (state == NULL || storage == NULL || item_size == 0u || capacity == 0u) return -1;
    memset(queue, 0, sizeof(*queue));
    if (pthread_mutex_init(&state->mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&state->not_empty, NULL) != 0 ||
        pthread_cond_init(&state->not_full, NULL) != 0) {
        pthread_cond_destroy(&state->not_empty);
        pthread_mutex_destroy(&state->mutex);
        return -1;
    }
    state->storage = storage;
    state->item_size = item_size;
    state->capacity = capacity;
    return 0;
}
static void os_queue_destroy(void *context, wlh_osal_queue_t *queue) {
    queue_state_t *state = (queue_state_t *)queue;
    (void)context;
    if (state == NULL) return;
    (void)pthread_cond_destroy(&state->not_empty);
    (void)pthread_cond_destroy(&state->not_full);
    (void)pthread_mutex_destroy(&state->mutex);
}
static int os_queue_send(void *context, wlh_osal_queue_t *queue, const void *item,
                         uint32_t timeout_ms) {
    queue_state_t *state = (queue_state_t *)queue;
    size_t tail;
    (void)context;
    if (state == NULL || item == NULL || pthread_mutex_lock(&state->mutex) != 0) return -1;
    while (state->count == state->capacity) {
        if (wait_condition(&state->not_full, &state->mutex, timeout_ms) != 0) {
            pthread_mutex_unlock(&state->mutex);
            return -1;
        }
    }
    tail = (state->head + state->count) % state->capacity;
    memcpy(state->storage + tail * state->item_size, item, state->item_size);
    state->count++;
    pthread_cond_signal(&state->not_empty);
    pthread_mutex_unlock(&state->mutex);
    return 0;
}
static int os_queue_send_from_isr(void *context, wlh_osal_queue_t *queue,
                                  const void *item,
                                  bool *higher_priority_task_woken) {
    if (higher_priority_task_woken != NULL) *higher_priority_task_woken = true;
    return os_queue_send(context, queue, item, WLH_OSAL_NO_WAIT);
}
static int os_queue_receive(void *context, wlh_osal_queue_t *queue, void *item,
                            uint32_t timeout_ms) {
    queue_state_t *state = (queue_state_t *)queue;
    (void)context;
    if (state == NULL || item == NULL || pthread_mutex_lock(&state->mutex) != 0) return -1;
    while (state->count == 0u) {
        if (wait_condition(&state->not_empty, &state->mutex, timeout_ms) != 0) {
            pthread_mutex_unlock(&state->mutex);
            return -1;
        }
    }
    memcpy(item, state->storage + state->head * state->item_size, state->item_size);
    state->head = (state->head + 1u) % state->capacity;
    state->count--;
    pthread_cond_signal(&state->not_full);
    pthread_mutex_unlock(&state->mutex);
    return 0;
}

static void *timer_main(void *argument) {
    timer_state_t *state = argument;
    pthread_mutex_lock(&state->mutex);
    while (!state->stopping) {
        uint64_t now;
        uint32_t wait_ms;
        while (!state->armed && !state->stopping)
            pthread_cond_wait(&state->condition, &state->mutex);
        if (state->stopping) break;
        now = clock_ms();
        if (now < state->deadline_ms) {
            wait_ms = (uint32_t)(state->deadline_ms - now);
            struct timespec deadline = realtime_after(wait_ms);
            (void)pthread_cond_timedwait(&state->condition, &state->mutex, &deadline);
            continue;
        }
        if (state->periodic) state->deadline_ms = now + state->period_ms;
        else state->armed = false;
        pthread_mutex_unlock(&state->mutex);
        state->callback(state->argument);
        pthread_mutex_lock(&state->mutex);
    }
    pthread_mutex_unlock(&state->mutex);
    return NULL;
}
static int os_timer_create(void *context, wlh_osal_timer_t *timer,
                           wlh_osal_timer_fn callback, void *argument) {
    timer_state_t *state = (timer_state_t *)timer;
    (void)context;
    if (state == NULL || callback == NULL) return -1;
    memset(timer, 0, sizeof(*timer));
    if (pthread_mutex_init(&state->mutex, NULL) != 0) return -1;
    if (pthread_cond_init(&state->condition, NULL) != 0) {
        pthread_mutex_destroy(&state->mutex);
        return -1;
    }
    state->callback = callback;
    state->argument = argument;
    if (pthread_create(&state->thread, NULL, timer_main, state) != 0) {
        pthread_cond_destroy(&state->condition);
        pthread_mutex_destroy(&state->mutex);
        return -1;
    }
    state->created = true;
    return 0;
}
static void os_timer_destroy(void *context, wlh_osal_timer_t *timer) {
    timer_state_t *state = (timer_state_t *)timer;
    (void)context;
    if (state == NULL || !state->created) return;
    pthread_mutex_lock(&state->mutex);
    state->stopping = true;
    pthread_cond_signal(&state->condition);
    pthread_mutex_unlock(&state->mutex);
    (void)pthread_join(state->thread, NULL);
    (void)pthread_cond_destroy(&state->condition);
    (void)pthread_mutex_destroy(&state->mutex);
    state->created = false;
}
static int os_timer_start(void *context, wlh_osal_timer_t *timer,
                          uint32_t period_ms, bool periodic) {
    timer_state_t *state = (timer_state_t *)timer;
    (void)context;
    if (state == NULL || !state->created || period_ms == 0u) return -1;
    pthread_mutex_lock(&state->mutex);
    state->period_ms = period_ms;
    state->periodic = periodic;
    state->deadline_ms = clock_ms() + period_ms;
    state->armed = true;
    pthread_cond_signal(&state->condition);
    pthread_mutex_unlock(&state->mutex);
    return 0;
}
static int os_timer_stop(void *context, wlh_osal_timer_t *timer) {
    timer_state_t *state = (timer_state_t *)timer;
    (void)context;
    if (state == NULL || !state->created) return -1;
    pthread_mutex_lock(&state->mutex);
    state->armed = false;
    pthread_cond_signal(&state->condition);
    pthread_mutex_unlock(&state->mutex);
    return 0;
}

static uint64_t os_monotonic(void *context) { (void)context; return clock_ms(); }
static void os_sleep(void *context, uint32_t duration_ms) { (void)context; sleep_duration(duration_ms); }
static void os_yield(void *context) { (void)context; sched_yield(); }
static bool os_in_isr(void *context) { (void)context; return false; }

void wlh_posix_osal_init(wlh_posix_osal_t *osal) {
    if (osal != NULL) memset(osal, 0, sizeof(*osal));
}

wlh_osal_ops_t wlh_posix_osal_ops(wlh_posix_osal_t *osal) {
    wlh_osal_ops_t ops = {
        osal, os_task_create, os_task_join,
        os_mutex_create, os_mutex_destroy, os_mutex_lock, os_mutex_unlock,
        os_semaphore_create, os_semaphore_destroy, os_semaphore_take,
        os_semaphore_give, os_semaphore_give_from_isr,
        os_event_create, os_event_destroy, os_event_wait, os_event_set,
        os_event_set_from_isr, os_queue_create, os_queue_destroy,
        os_queue_send, os_queue_send_from_isr, os_queue_receive,
        os_timer_create, os_timer_destroy, os_timer_start, os_timer_stop,
        os_monotonic, os_sleep, os_yield, os_in_isr
    };
    return ops;
}
