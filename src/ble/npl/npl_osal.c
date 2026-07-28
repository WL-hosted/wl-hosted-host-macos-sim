/* NimBLE Porting Layer backed exclusively by wlh::osal. No direct pthread or
   other platform dependencies may appear here; the POSIX adapter owns those. */

#include "nimble/nimble_npl.h"

#include "wlh_npl.h"

#include <stdatomic.h>
#include <stddef.h>

#define NPL_MAGIC 0x4e504c31u

static const wlh_osal_ops_t *npl_ops;
static atomic_bool npl_started;
static wlh_osal_mutex_t npl_critical;
static bool npl_critical_ready;
static _Thread_local uint32_t npl_critical_depth;
static _Thread_local uint8_t npl_task_marker;

int wlh_npl_init(const wlh_osal_ops_t *ops) {
    if (ops == NULL || !wlh_osal_ops_valid(ops))
        return -1;
    if (npl_ops != NULL)
        return npl_ops == ops ? 0 : -1;
    if (ops->mutex_create(ops->context, &npl_critical) != 0)
        return -1;
    npl_critical_ready = true;
    npl_ops = ops;
    return 0;
}

void wlh_npl_deinit(void) {
    if (npl_ops == NULL)
        return;
    atomic_store(&npl_started, false);
    if (npl_critical_ready)
        npl_ops->mutex_destroy(npl_ops->context, &npl_critical);
    npl_critical_ready = false;
    npl_ops = NULL;
}

void wlh_npl_set_os_started(bool started) {
    atomic_store(&npl_started, started);
}

bool ble_npl_os_started(void) {
    return atomic_load(&npl_started);
}

void *ble_npl_get_current_task_id(void) {
    /* The address of a thread-local object is stable and unique per task. */
    return &npl_task_marker;
}

/*
 * Event queue: intrusive doubly-linked list guarded by a mutex plus a
 * counting semaphore. eventq_remove may leave a surplus semaphore token
 * behind; ble_npl_eventq_get treats an empty list after a successful take as
 * a spurious wakeup and keeps waiting for the remaining timeout.
 */

static void eventq_teardown(struct ble_npl_eventq *evq) {
    npl_ops->mutex_destroy(npl_ops->context, &evq->lock);
    npl_ops->semaphore_destroy(npl_ops->context, &evq->ready);
    evq->magic = 0u;
}

void ble_npl_eventq_init(struct ble_npl_eventq *evq) {
    if (evq->magic == NPL_MAGIC)
        eventq_teardown(evq);
    if (npl_ops->mutex_create(npl_ops->context, &evq->lock) != 0)
        return;
    if (npl_ops->semaphore_create(
            npl_ops->context, &evq->ready, 0u, UINT16_MAX
        ) != 0) {
        npl_ops->mutex_destroy(npl_ops->context, &evq->lock);
        return;
    }
    evq->head = NULL;
    evq->tail = NULL;
    evq->magic = NPL_MAGIC;
}

static struct ble_npl_event *eventq_pop_locked(struct ble_npl_eventq *evq) {
    struct ble_npl_event *ev = evq->head;
    if (ev == NULL)
        return NULL;
    evq->head = ev->ev_next;
    if (evq->head != NULL)
        evq->head->ev_prev = NULL;
    else
        evq->tail = NULL;
    ev->ev_next = NULL;
    ev->ev_prev = NULL;
    ev->ev_queued = 0u;
    return ev;
}

struct ble_npl_event *ble_npl_eventq_get(
    struct ble_npl_eventq *evq, ble_npl_time_t tmo
) {
    uint64_t deadline = 0u;
    bool forever = tmo == BLE_NPL_TIME_FOREVER;

    if (evq->magic != NPL_MAGIC)
        return NULL;
    if (!forever)
        deadline = npl_ops->monotonic_time_ms(npl_ops->context) + tmo;
    for (;;) {
        struct ble_npl_event *ev;
        uint32_t wait_ms = WLH_OSAL_WAIT_FOREVER;
        if (!forever) {
            uint64_t now = npl_ops->monotonic_time_ms(npl_ops->context);
            wait_ms = now < deadline ? (uint32_t)(deadline - now) : 0u;
        }
        if (npl_ops->semaphore_take(npl_ops->context, &evq->ready, wait_ms) !=
            0)
            return NULL;
        npl_ops->mutex_lock(
            npl_ops->context, &evq->lock, WLH_OSAL_WAIT_FOREVER
        );
        ev = eventq_pop_locked(evq);
        npl_ops->mutex_unlock(npl_ops->context, &evq->lock);
        if (ev != NULL)
            return ev;
        /* Surplus token left by eventq_remove; retry within the deadline. */
        if (!forever &&
            npl_ops->monotonic_time_ms(npl_ops->context) >= deadline)
            return NULL;
    }
}

void ble_npl_eventq_put(struct ble_npl_eventq *evq, struct ble_npl_event *ev) {
    bool queued = false;
    if (evq->magic != NPL_MAGIC)
        return;
    npl_ops->mutex_lock(npl_ops->context, &evq->lock, WLH_OSAL_WAIT_FOREVER);
    if (!ev->ev_queued) {
        ev->ev_queued = 1u;
        ev->ev_next = NULL;
        ev->ev_prev = evq->tail;
        if (evq->tail != NULL)
            evq->tail->ev_next = ev;
        else
            evq->head = ev;
        evq->tail = ev;
        queued = true;
    }
    npl_ops->mutex_unlock(npl_ops->context, &evq->lock);
    if (queued)
        (void)npl_ops->semaphore_give(npl_ops->context, &evq->ready);
}

void ble_npl_eventq_remove(
    struct ble_npl_eventq *evq, struct ble_npl_event *ev
) {
    if (evq->magic != NPL_MAGIC)
        return;
    npl_ops->mutex_lock(npl_ops->context, &evq->lock, WLH_OSAL_WAIT_FOREVER);
    if (ev->ev_queued) {
        if (ev->ev_prev != NULL)
            ev->ev_prev->ev_next = ev->ev_next;
        else
            evq->head = ev->ev_next;
        if (ev->ev_next != NULL)
            ev->ev_next->ev_prev = ev->ev_prev;
        else
            evq->tail = ev->ev_prev;
        ev->ev_next = NULL;
        ev->ev_prev = NULL;
        ev->ev_queued = 0u;
    }
    npl_ops->mutex_unlock(npl_ops->context, &evq->lock);
}

void ble_npl_event_init(
    struct ble_npl_event *ev, ble_npl_event_fn *fn, void *arg
) {
    ev->ev_queued = 0u;
    ev->ev_cb = fn;
    ev->ev_arg = arg;
    ev->ev_prev = NULL;
    ev->ev_next = NULL;
}

bool ble_npl_event_is_queued(struct ble_npl_event *ev) {
    return ev->ev_queued != 0u;
}

void *ble_npl_event_get_arg(struct ble_npl_event *ev) {
    return ev->ev_arg;
}

void ble_npl_event_set_arg(struct ble_npl_event *ev, void *arg) {
    ev->ev_arg = arg;
}

bool ble_npl_eventq_is_empty(struct ble_npl_eventq *evq) {
    bool empty;
    if (evq->magic != NPL_MAGIC)
        return true;
    npl_ops->mutex_lock(npl_ops->context, &evq->lock, WLH_OSAL_WAIT_FOREVER);
    empty = evq->head == NULL;
    npl_ops->mutex_unlock(npl_ops->context, &evq->lock);
    return empty;
}

void ble_npl_event_run(struct ble_npl_event *ev) {
    ev->ev_cb(ev);
}

/*
 * Mutex: recursion is tracked explicitly because the OSAL contract does not
 * promise recursive semantics. Reading mu->owner without the lock is safe:
 * only the current owner can have stored our own task id there.
 */

ble_npl_error_t ble_npl_mutex_init(struct ble_npl_mutex *mu) {
    if (mu->magic == NPL_MAGIC) {
        npl_ops->mutex_destroy(npl_ops->context, &mu->lock);
        mu->magic = 0u;
    }
    if (npl_ops->mutex_create(npl_ops->context, &mu->lock) != 0)
        return BLE_NPL_ERROR;
    mu->owner = NULL;
    mu->depth = 0u;
    mu->magic = NPL_MAGIC;
    return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_mutex_pend(
    struct ble_npl_mutex *mu, ble_npl_time_t timeout
) {
    void *self = ble_npl_get_current_task_id();
    if (mu->magic != NPL_MAGIC)
        return BLE_NPL_BAD_MUTEX;
    if (mu->owner == self) {
        mu->depth++;
        return BLE_NPL_OK;
    }
    if (npl_ops->mutex_lock(
            npl_ops->context,
            &mu->lock,
            timeout == BLE_NPL_TIME_FOREVER ? WLH_OSAL_WAIT_FOREVER : timeout
        ) != 0)
        return BLE_NPL_TIMEOUT;
    mu->owner = self;
    mu->depth = 1u;
    return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_mutex_release(struct ble_npl_mutex *mu) {
    if (mu->magic != NPL_MAGIC || mu->owner != ble_npl_get_current_task_id() ||
        mu->depth == 0u)
        return BLE_NPL_BAD_MUTEX;
    if (--mu->depth == 0u) {
        mu->owner = NULL;
        npl_ops->mutex_unlock(npl_ops->context, &mu->lock);
    }
    return BLE_NPL_OK;
}

/*
 * Semaphore: the OSAL does not expose a count query, so a shadow counter is
 * maintained under its own lock.
 */

ble_npl_error_t ble_npl_sem_init(struct ble_npl_sem *sem, uint16_t tokens) {
    if (sem->magic == NPL_MAGIC) {
        npl_ops->semaphore_destroy(npl_ops->context, &sem->sem);
        npl_ops->mutex_destroy(npl_ops->context, &sem->lock);
        sem->magic = 0u;
    }
    if (npl_ops->semaphore_create(
            npl_ops->context, &sem->sem, tokens, UINT16_MAX
        ) != 0)
        return BLE_NPL_ERROR;
    if (npl_ops->mutex_create(npl_ops->context, &sem->lock) != 0) {
        npl_ops->semaphore_destroy(npl_ops->context, &sem->sem);
        return BLE_NPL_ERROR;
    }
    sem->count = tokens;
    sem->magic = NPL_MAGIC;
    return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_sem_pend(
    struct ble_npl_sem *sem, ble_npl_time_t timeout
) {
    if (sem->magic != NPL_MAGIC)
        return BLE_NPL_ERROR;
    if (npl_ops->semaphore_take(
            npl_ops->context,
            &sem->sem,
            timeout == BLE_NPL_TIME_FOREVER ? WLH_OSAL_WAIT_FOREVER : timeout
        ) != 0)
        return BLE_NPL_TIMEOUT;
    npl_ops->mutex_lock(npl_ops->context, &sem->lock, WLH_OSAL_WAIT_FOREVER);
    if (sem->count > 0u)
        sem->count--;
    npl_ops->mutex_unlock(npl_ops->context, &sem->lock);
    return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_sem_release(struct ble_npl_sem *sem) {
    if (sem->magic != NPL_MAGIC)
        return BLE_NPL_ERROR;
    npl_ops->mutex_lock(npl_ops->context, &sem->lock, WLH_OSAL_WAIT_FOREVER);
    sem->count++;
    npl_ops->mutex_unlock(npl_ops->context, &sem->lock);
    if (npl_ops->semaphore_give(npl_ops->context, &sem->sem) != 0) {
        npl_ops->mutex_lock(
            npl_ops->context, &sem->lock, WLH_OSAL_WAIT_FOREVER
        );
        sem->count--;
        npl_ops->mutex_unlock(npl_ops->context, &sem->lock);
        return BLE_NPL_ERROR;
    }
    return BLE_NPL_OK;
}

uint16_t ble_npl_sem_get_count(struct ble_npl_sem *sem) {
    uint16_t count;
    if (sem->magic != NPL_MAGIC)
        return 0u;
    npl_ops->mutex_lock(npl_ops->context, &sem->lock, WLH_OSAL_WAIT_FOREVER);
    count = sem->count;
    npl_ops->mutex_unlock(npl_ops->context, &sem->lock);
    return count;
}

/*
 * Callout: an OSAL one-shot timer that posts the event into the target
 * queue. The lock orders reset/stop against the timer callback; the OSAL
 * timer thread never holds it while posting.
 */

static void callout_timer_fired(void *argument) {
    struct ble_npl_callout *co = argument;
    bool fire;
    npl_ops->mutex_lock(npl_ops->context, &co->lock, WLH_OSAL_WAIT_FOREVER);
    fire = co->active;
    co->active = false;
    npl_ops->mutex_unlock(npl_ops->context, &co->lock);
    if (fire)
        ble_npl_eventq_put(co->c_evq, &co->c_ev);
}

void ble_npl_callout_init(
    struct ble_npl_callout *co,
    struct ble_npl_eventq *evq,
    ble_npl_event_fn *ev_cb,
    void *ev_arg
) {
    if (co->magic == NPL_MAGIC) {
        npl_ops->timer_stop(npl_ops->context, &co->timer);
        npl_ops->timer_destroy(npl_ops->context, &co->timer);
        npl_ops->mutex_destroy(npl_ops->context, &co->lock);
        co->magic = 0u;
    }
    ble_npl_event_init(&co->c_ev, ev_cb, ev_arg);
    co->c_evq = evq;
    co->active = false;
    co->expiry = 0u;
    if (npl_ops->mutex_create(npl_ops->context, &co->lock) != 0)
        return;
    if (npl_ops->timer_create(
            npl_ops->context, &co->timer, callout_timer_fired, co
        ) != 0) {
        npl_ops->mutex_destroy(npl_ops->context, &co->lock);
        return;
    }
    co->magic = NPL_MAGIC;
}

ble_npl_error_t ble_npl_callout_reset(
    struct ble_npl_callout *co, ble_npl_time_t ticks
) {
    if (co->magic != NPL_MAGIC)
        return BLE_NPL_EINVAL;
    npl_ops->mutex_lock(npl_ops->context, &co->lock, WLH_OSAL_WAIT_FOREVER);
    npl_ops->timer_stop(npl_ops->context, &co->timer);
    co->expiry =
        (ble_npl_time_t)npl_ops->monotonic_time_ms(npl_ops->context) + ticks;
    if (ticks == 0u) {
        co->active = false;
        npl_ops->mutex_unlock(npl_ops->context, &co->lock);
        ble_npl_eventq_put(co->c_evq, &co->c_ev);
        return BLE_NPL_OK;
    }
    co->active = true;
    if (npl_ops->timer_start(npl_ops->context, &co->timer, ticks, false) != 0) {
        co->active = false;
        npl_ops->mutex_unlock(npl_ops->context, &co->lock);
        return BLE_NPL_ERROR;
    }
    npl_ops->mutex_unlock(npl_ops->context, &co->lock);
    return BLE_NPL_OK;
}

void ble_npl_callout_stop(struct ble_npl_callout *co) {
    if (co->magic != NPL_MAGIC)
        return;
    npl_ops->mutex_lock(npl_ops->context, &co->lock, WLH_OSAL_WAIT_FOREVER);
    npl_ops->timer_stop(npl_ops->context, &co->timer);
    co->active = false;
    npl_ops->mutex_unlock(npl_ops->context, &co->lock);
    if (co->c_evq != NULL)
        ble_npl_eventq_remove(co->c_evq, &co->c_ev);
}

bool ble_npl_callout_is_active(struct ble_npl_callout *co) {
    bool active;
    if (co->magic != NPL_MAGIC)
        return false;
    npl_ops->mutex_lock(npl_ops->context, &co->lock, WLH_OSAL_WAIT_FOREVER);
    active = co->active;
    npl_ops->mutex_unlock(npl_ops->context, &co->lock);
    return active;
}

ble_npl_time_t ble_npl_callout_get_ticks(struct ble_npl_callout *co) {
    return co->expiry;
}

ble_npl_time_t ble_npl_callout_remaining_ticks(
    struct ble_npl_callout *co, ble_npl_time_t time
) {
    ble_npl_stime_t remaining = (ble_npl_stime_t)(co->expiry - time);
    return remaining > 0 ? (ble_npl_time_t)remaining : 0u;
}

void ble_npl_callout_set_arg(struct ble_npl_callout *co, void *arg) {
    co->c_ev.ev_arg = arg;
}

/*
 * Time: ticks are milliseconds, so conversions only need to saturate.
 */

ble_npl_time_t ble_npl_time_get(void) {
    return (ble_npl_time_t)npl_ops->monotonic_time_ms(npl_ops->context);
}

ble_npl_error_t ble_npl_time_ms_to_ticks(
    uint32_t ms, ble_npl_time_t *out_ticks
) {
    *out_ticks = ms;
    return BLE_NPL_OK;
}

ble_npl_error_t ble_npl_time_ticks_to_ms(
    ble_npl_time_t ticks, uint32_t *out_ms
) {
    *out_ms = ticks;
    return BLE_NPL_OK;
}

ble_npl_time_t ble_npl_time_ms_to_ticks32(uint32_t ms) {
    return ms;
}

uint32_t ble_npl_time_ticks_to_ms32(ble_npl_time_t ticks) {
    return ticks;
}

void ble_npl_time_delay(ble_npl_time_t ticks) {
    npl_ops->sleep_ms(npl_ops->context, ticks);
}

/*
 * Critical section: one process-wide lock with a per-task depth counter so
 * nesting inside a single task never deadlocks.
 */

uint32_t ble_npl_hw_enter_critical(void) {
    if (npl_critical_depth == 0u)
        npl_ops->mutex_lock(
            npl_ops->context, &npl_critical, WLH_OSAL_WAIT_FOREVER
        );
    npl_critical_depth++;
    return 0u;
}

void ble_npl_hw_exit_critical(uint32_t ctx) {
    (void)ctx;
    if (npl_critical_depth == 0u)
        return;
    if (--npl_critical_depth == 0u)
        npl_ops->mutex_unlock(npl_ops->context, &npl_critical);
}

bool ble_npl_hw_is_in_critical(void) {
    return npl_critical_depth > 0u;
}
