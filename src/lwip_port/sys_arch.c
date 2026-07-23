#include "sys_arch.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct thread_start {
    lwip_thread_fn entry;
    void *argument;
} thread_start_t;

static wlh_osal_ops_t osal;
static pthread_mutex_t protection;
static pthread_once_t protection_once = PTHREAD_ONCE_INIT;
static _Thread_local unsigned protection_depth;

static void initialize_protection(void) {
    pthread_mutexattr_t attributes;
    (void)pthread_mutexattr_init(&attributes);
    (void)pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE);
    (void)pthread_mutex_init(&protection, &attributes);
    (void)pthread_mutexattr_destroy(&attributes);
}

void wlh_lwip_sys_arch_configure(const wlh_osal_ops_t *ops) {
    LWIP_ASSERT("valid OSAL", wlh_osal_ops_valid(ops));
    osal = *ops;
}

void sys_init(void) {
    LWIP_ASSERT("sys_arch configured", wlh_osal_ops_valid(&osal));
    (void)pthread_once(&protection_once, initialize_protection);
}

u32_t sys_now(void) {
    return (u32_t)osal.monotonic_time_ms(osal.context);
}

u32_t sys_jiffies(void) {
    return sys_now();
}

void sys_msleep(u32_t milliseconds) {
    osal.sleep_ms(osal.context, milliseconds);
}

err_t sys_mutex_new(sys_mutex_t *mutex) {
    if (mutex == NULL || osal.mutex_create(osal.context, &mutex->object) != 0)
        return ERR_MEM;
    mutex->valid = true;
    return ERR_OK;
}

void sys_mutex_lock(sys_mutex_t *mutex) {
    LWIP_ASSERT("valid mutex", mutex != NULL && mutex->valid);
    LWIP_ASSERT(
        "mutex lock",
        osal.mutex_lock(osal.context, &mutex->object, WLH_OSAL_WAIT_FOREVER) ==
            0
    );
}

void sys_mutex_unlock(sys_mutex_t *mutex) {
    LWIP_ASSERT("valid mutex", mutex != NULL && mutex->valid);
    osal.mutex_unlock(osal.context, &mutex->object);
}

void sys_mutex_free(sys_mutex_t *mutex) {
    if (mutex != NULL && mutex->valid) {
        osal.mutex_destroy(osal.context, &mutex->object);
        mutex->valid = false;
    }
}

int sys_mutex_valid(sys_mutex_t *mutex) {
    return mutex != NULL && mutex->valid;
}

void sys_mutex_set_invalid(sys_mutex_t *mutex) {
    if (mutex != NULL)
        mutex->valid = false;
}

err_t sys_sem_new(sys_sem_t *sem, u8_t count) {
    if (sem == NULL ||
        osal.semaphore_create(osal.context, &sem->object, count, 1u) != 0)
        return ERR_MEM;
    sem->valid = true;
    return ERR_OK;
}

void sys_sem_signal(sys_sem_t *sem) {
    LWIP_ASSERT("valid semaphore", sem != NULL && sem->valid);
    LWIP_ASSERT(
        "semaphore give", osal.semaphore_give(osal.context, &sem->object) == 0
    );
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout) {
    uint32_t wait = timeout == 0u ? WLH_OSAL_WAIT_FOREVER : timeout;
    uint32_t started = sys_now();
    LWIP_ASSERT("valid semaphore", sem != NULL && sem->valid);
    if (osal.semaphore_take(osal.context, &sem->object, wait) != 0)
        return SYS_ARCH_TIMEOUT;
    return sys_now() - started;
}

void sys_sem_free(sys_sem_t *sem) {
    if (sem != NULL && sem->valid) {
        osal.semaphore_destroy(osal.context, &sem->object);
        sem->valid = false;
    }
}

int sys_sem_valid(sys_sem_t *sem) {
    return sem != NULL && sem->valid;
}

void sys_sem_set_invalid(sys_sem_t *sem) {
    if (sem != NULL)
        sem->valid = false;
}

err_t sys_mbox_new(sys_mbox_t *mbox, int size) {
    size_t capacity;
    if (mbox == NULL || size <= 0)
        return ERR_ARG;
    capacity = (size_t)size;
    mbox->storage = calloc(capacity, sizeof(*mbox->storage));
    if (mbox->storage == NULL)
        return ERR_MEM;
    if (osal.queue_create(
            osal.context,
            &mbox->object,
            mbox->storage,
            sizeof(*mbox->storage),
            capacity
        ) != 0) {
        free(mbox->storage);
        mbox->storage = NULL;
        return ERR_MEM;
    }
    mbox->valid = true;
    return ERR_OK;
}

void sys_mbox_post(sys_mbox_t *mbox, void *message) {
    LWIP_ASSERT("valid mailbox", mbox != NULL && mbox->valid);
    LWIP_ASSERT(
        "mailbox post",
        osal.queue_send(
            osal.context, &mbox->object, &message, WLH_OSAL_WAIT_FOREVER
        ) == 0
    );
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *message) {
    if (mbox == NULL || !mbox->valid)
        return ERR_ARG;
    return osal.queue_send(
               osal.context, &mbox->object, &message, WLH_OSAL_NO_WAIT
           ) == 0
               ? ERR_OK
               : ERR_MEM;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *message) {
    bool woken = false;
    if (mbox == NULL || !mbox->valid)
        return ERR_ARG;
    return osal.queue_send_from_isr(
               osal.context, &mbox->object, &message, &woken
           ) == 0
               ? ERR_OK
               : ERR_MEM;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **message, u32_t timeout) {
    void *received = NULL;
    uint32_t wait = timeout == 0u ? WLH_OSAL_WAIT_FOREVER : timeout;
    uint32_t started = sys_now();
    LWIP_ASSERT("valid mailbox", mbox != NULL && mbox->valid);
    if (osal.queue_receive(osal.context, &mbox->object, &received, wait) != 0)
        return SYS_ARCH_TIMEOUT;
    if (message != NULL)
        *message = received;
    return sys_now() - started;
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **message) {
    void *received = NULL;
    if (mbox == NULL || !mbox->valid ||
        osal.queue_receive(
            osal.context, &mbox->object, &received, WLH_OSAL_NO_WAIT
        ) != 0)
        return SYS_MBOX_EMPTY;
    if (message != NULL)
        *message = received;
    return 0u;
}

void sys_mbox_free(sys_mbox_t *mbox) {
    if (mbox != NULL && mbox->valid) {
        osal.queue_destroy(osal.context, &mbox->object);
        free(mbox->storage);
        mbox->storage = NULL;
        mbox->valid = false;
    }
}

int sys_mbox_valid(sys_mbox_t *mbox) {
    return mbox != NULL && mbox->valid;
}

void sys_mbox_set_invalid(sys_mbox_t *mbox) {
    if (mbox != NULL)
        mbox->valid = false;
}

static void thread_entry(void *argument) {
    thread_start_t *start = argument;
    lwip_thread_fn entry = start->entry;
    void *entry_argument = start->argument;
    free(start);
    entry(entry_argument);
}

sys_thread_t sys_thread_new(
    const char *name,
    lwip_thread_fn entry,
    void *argument,
    int stack_size,
    int priority
) {
    wlh_osal_task_t *task = calloc(1u, sizeof(*task));
    thread_start_t *start = calloc(1u, sizeof(*start));
    wlh_osal_task_attributes_t attributes;
    LWIP_ASSERT("thread allocation", task != NULL && start != NULL);
    start->entry = entry;
    start->argument = argument;
    attributes.name = name;
    attributes.stack_size = stack_size > 0 ? (size_t)stack_size : 0u;
    attributes.priority = priority;
    LWIP_ASSERT(
        "thread create",
        osal.task_create(
            osal.context, task, &attributes, thread_entry, start
        ) == 0
    );
    return task;
}

sys_prot_t sys_arch_protect(void) {
    (void)pthread_once(&protection_once, initialize_protection);
    (void)pthread_mutex_lock(&protection);
    return protection_depth++;
}

void sys_arch_unprotect(sys_prot_t previous) {
    LWIP_ASSERT("balanced protection", protection_depth > 0u);
    protection_depth = (unsigned)previous;
    (void)pthread_mutex_unlock(&protection);
}
