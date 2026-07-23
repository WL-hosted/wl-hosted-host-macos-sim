#ifndef WLH_HOST_SIM_H
#define WLH_HOST_SIM_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SIM_IPC_HELLO_SIZE 16u
#define SIM_IPC_MAX_RECORD_SIZE 65563u
#define SIM_IPC_SIDEBAND_FLAG 0x01u

enum sim_role { SIM_ROLE_HOST = 1, SIM_ROLE_COPROC = 2, SIM_ROLE_MANAGER = 3 };
enum sim_record_kind {
    SIM_RECORD_WIRE_FRAME = 1,
    SIM_RECORD_RUNTIME_INFO = 2,
    SIM_RECORD_FAULT_REQUEST = 3,
    SIM_RECORD_FAULT_RESPONSE = 4,
    SIM_RECORD_WIFI_COMMAND = 5,
    SIM_RECORD_PING_COMMAND = 6,
    SIM_RECORD_PING_RESULT = 7
};

typedef struct sim_ipc {
    int fd;
    enum sim_role peer_role;
    uint32_t max_record_size;
    bool sideband;
    pthread_mutex_t write_mutex;
} sim_ipc_t;

int sim_ipc_open(sim_ipc_t *ipc, const char *endpoint);
void sim_ipc_close(sim_ipc_t *ipc);
int sim_ipc_write(
    sim_ipc_t *ipc, uint8_t kind, const uint8_t *payload, size_t payload_size
);
int sim_ipc_read(
    sim_ipc_t *ipc, uint8_t *kind, uint8_t **payload, size_t *payload_size
);

typedef void (*sim_task_fn)(void *context);

typedef struct sim_task {
    sim_task_fn function;
    void *context;
} sim_task_t;

typedef struct sim_executor {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    sim_task_t queue[64];
    size_t head;
    size_t count;
    bool stopping;
} sim_executor_t;

int sim_executor_start(sim_executor_t *executor);
void sim_executor_stop(sim_executor_t *executor);
int sim_executor_post(void *context, sim_task_fn function, void *task_context);

#endif
