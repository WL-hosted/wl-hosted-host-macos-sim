#ifndef WLH_LWIP_ARCH_SYS_ARCH_H
#define WLH_LWIP_ARCH_SYS_ARCH_H

#include "wlh/osal.h"

#include <stdbool.h>

typedef struct sys_sem {
    wlh_osal_semaphore_t object;
    bool valid;
} sys_sem_t;

typedef struct sys_mutex {
    wlh_osal_mutex_t object;
    bool valid;
} sys_mutex_t;

typedef struct sys_mbox {
    wlh_osal_queue_t object;
    void **storage;
    bool valid;
} sys_mbox_t;

typedef wlh_osal_task_t *sys_thread_t;
typedef unsigned long sys_prot_t;

#endif
