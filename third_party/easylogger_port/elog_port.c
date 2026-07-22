#include <elog.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t output_lock;

ElogErrCode elog_port_init(void) {
    pthread_mutex_init(&output_lock, NULL);
    return ELOG_NO_ERR;
}

void elog_port_deinit(void) {
    pthread_mutex_destroy(&output_lock);
}

void elog_port_output(const char *log, size_t size) {
    fwrite(log, 1u, size, stderr);
}

void elog_port_output_lock(void) {
    pthread_mutex_lock(&output_lock);
}

void elog_port_output_unlock(void) {
    pthread_mutex_unlock(&output_lock);
}

const char *elog_port_get_time(void) {
    static char cur_system_time[24];
    struct timespec ts;
    struct tm cur_tm;

    memset(cur_system_time, 0, sizeof(cur_system_time));
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &cur_tm);
    strftime(cur_system_time, sizeof(cur_system_time), "%Y-%m-%d %T", &cur_tm);
    return cur_system_time;
}

const char *elog_port_get_p_info(void) {
    static char cur_process_info[16];
    snprintf(cur_process_info, sizeof(cur_process_info), "pid:%d", getpid());
    return cur_process_info;
}

const char *elog_port_get_t_info(void) {
    static char cur_thread_info[16];
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    snprintf(cur_thread_info, sizeof(cur_thread_info), "tid:%llu", (unsigned long long)tid);
    return cur_thread_info;
}
