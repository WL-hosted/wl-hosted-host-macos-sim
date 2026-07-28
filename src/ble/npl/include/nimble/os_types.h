#ifndef WLH_NPL_OS_TYPES_H
#define WLH_NPL_OS_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "wlh/osal.h"

/* 1 NPL tick == 1 millisecond on this port. */
#define OS_TICKS_PER_SEC 1000

typedef uint32_t ble_npl_time_t;
typedef int32_t ble_npl_stime_t;

struct ble_npl_event {
    uint8_t ev_queued;
    ble_npl_event_fn *ev_cb;
    void *ev_arg;
    struct ble_npl_event *ev_prev;
    struct ble_npl_event *ev_next;
};

struct ble_npl_eventq {
    uint32_t magic;
    wlh_osal_mutex_t lock;
    wlh_osal_semaphore_t ready;
    struct ble_npl_event *head;
    struct ble_npl_event *tail;
};

struct ble_npl_callout {
    uint32_t magic;
    struct ble_npl_event c_ev;
    struct ble_npl_eventq *c_evq;
    wlh_osal_mutex_t lock;
    wlh_osal_timer_t timer;
    bool active;
    ble_npl_time_t expiry;
};

struct ble_npl_mutex {
    uint32_t magic;
    wlh_osal_mutex_t lock;
    void *owner;
    uint32_t depth;
};

struct ble_npl_sem {
    uint32_t magic;
    wlh_osal_semaphore_t sem;
    wlh_osal_mutex_t lock;
    uint16_t count;
};

#endif
