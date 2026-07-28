/* Unit tests for the WL-hosted NimBLE Porting Layer (NPL) over wlh::osal.
 *
 * Covers the behaviours the BLE plan calls out: event-queue FIFO ordering,
 * callout cancel/reschedule, one-shot timer expiry, semaphore pend timeout,
 * waking a blocked consumer (shutdown path), repeated start/stop cycling, and
 * millisecond<->tick conversion. Assertions stay live via -UNDEBUG.
 */

#include "nimble/nimble_npl.h"

#include "wlh/posix_osal.h"
#include "wlh_npl.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static wlh_posix_osal_t g_osal;
static wlh_osal_ops_t g_ops;

/* --- event-queue FIFO ordering ------------------------------------------- */

static int fifo_order[4];
static int fifo_count;

static void fifo_cb(struct ble_npl_event *ev) {
    fifo_order[fifo_count++] = (int)(intptr_t)ble_npl_event_get_arg(ev);
}

static void test_event_fifo(void) {
    /* NPL init routines treat a matching magic as "already initialised" and
       tear the old instance down first; NimBLE guarantees zeroed storage, so
       mirror that for stack objects. */
    struct ble_npl_eventq q = {0};
    struct ble_npl_event evs[3];
    int i;

    fifo_count = 0;
    ble_npl_eventq_init(&q);
    for (i = 0; i < 3; i++)
        ble_npl_event_init(&evs[i], fifo_cb, (void *)(intptr_t)(i + 1));

    for (i = 0; i < 3; i++) {
        ble_npl_eventq_put(&q, &evs[i]);
        assert(ble_npl_event_is_queued(&evs[i]));
    }
    assert(!ble_npl_eventq_is_empty(&q));

    for (i = 0; i < 3; i++) {
        struct ble_npl_event *ev = ble_npl_eventq_get(&q, 1000u);
        assert(ev != NULL);
        ble_npl_event_run(ev);
    }
    assert(ble_npl_eventq_is_empty(&q));
    assert(ble_npl_eventq_get(&q, 10u) == NULL); /* empty -> timeout */

    assert(fifo_count == 3);
    assert(fifo_order[0] == 1 && fifo_order[1] == 2 && fifo_order[2] == 3);
    printf("ok event_fifo\n");
}

/* --- callout cancel / reschedule ----------------------------------------- */

static int callout_fires;

static void callout_cb(struct ble_npl_event *ev) {
    (void)ev;
    callout_fires++;
}

static void test_callout_cancel_reschedule(void) {
    /* Callouts embed an OSAL timer thread and, like real firmware, are meant
       to be long-lived; keep them static so a leaked timer thread never
       observes a reused stack frame. */
    static struct ble_npl_eventq q;
    static struct ble_npl_callout co;
    struct ble_npl_event *ev;

    callout_fires = 0;
    ble_npl_eventq_init(&q);
    ble_npl_callout_init(&co, &q, callout_cb, NULL);

    /* Arm far out, then cancel before it can fire. */
    assert(ble_npl_callout_reset(&co, 10000u) == BLE_NPL_OK);
    assert(ble_npl_callout_is_active(&co));
    ble_npl_callout_stop(&co);
    assert(!ble_npl_callout_is_active(&co));
    assert(ble_npl_eventq_get(&q, 60u) == NULL); /* cancelled: nothing posts */

    /* Reschedule short; it must now fire exactly once. */
    assert(ble_npl_callout_reset(&co, 20u) == BLE_NPL_OK);
    ev = ble_npl_eventq_get(&q, 1000u);
    assert(ev != NULL);
    ble_npl_event_run(ev);
    assert(callout_fires == 1);
    assert(!ble_npl_callout_is_active(&co)); /* one-shot cleared after fire */
    ble_npl_callout_stop(&co);
    printf("ok callout_cancel_reschedule\n");
}

/* --- one-shot timer expiry timing ---------------------------------------- */

static void test_timer_expiry(void) {
    static struct ble_npl_eventq q;
    static struct ble_npl_callout co;
    struct ble_npl_event *ev;
    ble_npl_time_t start, elapsed;

    callout_fires = 0;
    ble_npl_eventq_init(&q);
    ble_npl_callout_init(&co, &q, callout_cb, NULL);

    start = ble_npl_time_get();
    assert(ble_npl_callout_reset(&co, 100u) == BLE_NPL_OK);
    ev = ble_npl_eventq_get(&q, 2000u);
    elapsed = ble_npl_time_get() - start;
    assert(ev != NULL);
    ble_npl_event_run(ev);
    assert(callout_fires == 1);
    /* Fire no earlier than requested; allow generous scheduler slack. */
    assert(elapsed >= 90u);
    assert(elapsed < 1000u);
    ble_npl_callout_stop(&co);
    printf("ok timer_expiry (%ums)\n", (unsigned)elapsed);
}

/* --- semaphore pend timeout ---------------------------------------------- */

static void test_sem_timeout(void) {
    struct ble_npl_sem sem = {0};
    ble_npl_time_t start, elapsed;

    assert(ble_npl_sem_init(&sem, 0u) == BLE_NPL_OK);
    assert(ble_npl_sem_get_count(&sem) == 0u);

    start = ble_npl_time_get();
    assert(ble_npl_sem_pend(&sem, 100u) == BLE_NPL_TIMEOUT);
    elapsed = ble_npl_time_get() - start;
    assert(elapsed >= 90u);

    /* A released token is takeable without blocking. */
    assert(ble_npl_sem_release(&sem) == BLE_NPL_OK);
    assert(ble_npl_sem_get_count(&sem) == 1u);
    assert(ble_npl_sem_pend(&sem, 100u) == BLE_NPL_OK);
    assert(ble_npl_sem_get_count(&sem) == 0u);
    printf("ok sem_timeout (%ums)\n", (unsigned)elapsed);
}

/* --- waking a blocked consumer (shutdown / cross-task signal) ------------- */

struct waiter_ctx {
    struct ble_npl_eventq *q;
    struct ble_npl_event *got;
};

static void *waiter_main(void *arg) {
    struct waiter_ctx *ctx = arg;
    ctx->got = ble_npl_eventq_get(ctx->q, BLE_NPL_TIME_FOREVER);
    return NULL;
}

static void test_shutdown_wakeup(void) {
    struct ble_npl_eventq q = {0};
    struct ble_npl_event ev;
    struct waiter_ctx ctx;
    pthread_t thread;

    ble_npl_eventq_init(&q);
    ble_npl_event_init(&ev, fifo_cb, (void *)(intptr_t)42);
    ctx.q = &q;
    ctx.got = NULL;

    assert(pthread_create(&thread, NULL, waiter_main, &ctx) == 0);
    ble_npl_time_delay(50u);     /* let the consumer block in FOREVER get */
    ble_npl_eventq_put(&q, &ev); /* the wakeup a shutdown would post */
    assert(pthread_join(thread, NULL) == 0);
    assert(ctx.got == &ev);
    printf("ok shutdown_wakeup\n");
}

/* --- repeated start/stop cycling ----------------------------------------- */

static void test_repeated_start_stop(void) {
    static struct ble_npl_eventq q;
    static struct ble_npl_callout co;
    int i;

    /* os_started flag must track both edges across many cycles. */
    for (i = 0; i < 100; i++) {
        wlh_npl_set_os_started(true);
        assert(ble_npl_os_started());
        wlh_npl_set_os_started(false);
        assert(!ble_npl_os_started());
    }

    /* Arm/cancel churn must not leak timers or wedge the queue. */
    ble_npl_eventq_init(&q);
    ble_npl_callout_init(&co, &q, callout_cb, NULL);
    for (i = 0; i < 100; i++) {
        assert(ble_npl_callout_reset(&co, 5000u) == BLE_NPL_OK);
        ble_npl_callout_stop(&co);
        assert(!ble_npl_callout_is_active(&co));
    }
    assert(ble_npl_eventq_get(&q, 20u) == NULL);
    ble_npl_callout_stop(&co);
    printf("ok repeated_start_stop\n");
}

/* --- millisecond <-> tick conversion ------------------------------------- */

static void test_tick_conversion(void) {
    ble_npl_time_t ticks = 0u;
    uint32_t ms = 0u;

    /* 1 tick == 1 ms on this port: conversions are identity + saturate. */
    assert(ble_npl_time_ms_to_ticks(1234u, &ticks) == BLE_NPL_OK);
    assert(ticks == 1234u);
    assert(ble_npl_time_ticks_to_ms(4321u, &ms) == BLE_NPL_OK);
    assert(ms == 4321u);
    assert(ble_npl_time_ms_to_ticks32(777u) == 777u);
    assert(ble_npl_time_ticks_to_ms32(888u) == 888u);
    assert(ble_npl_time_ms_to_ticks32(0u) == 0u);
    printf("ok tick_conversion\n");
}

int main(void) {
    wlh_posix_osal_init(&g_osal);
    g_ops = wlh_posix_osal_ops(&g_osal);
    assert(wlh_npl_init(&g_ops) == 0);
    assert(wlh_npl_init(&g_ops) == 0); /* idempotent with the same ops */

    test_event_fifo();
    test_callout_cancel_reschedule();
    test_timer_expiry();
    test_sem_timeout();
    test_shutdown_wakeup();
    test_repeated_start_stop();
    test_tick_conversion();

    wlh_npl_deinit();
    printf("all NPL tests passed\n");
    return 0;
}
