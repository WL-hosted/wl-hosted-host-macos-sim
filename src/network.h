#ifndef WLH_HOST_SIM_NETWORK_H
#define WLH_HOST_SIM_NETWORK_H

#include "wlh/osal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int (*sim_network_send_fn)(
    void *context, const uint8_t *frame, size_t size
);

typedef struct sim_ping_result {
    uint32_t request_id;
    char hostname[254];
    char address[47];
    uint32_t transmitted;
    uint32_t received;
    bool success;
    char detail[129];
} sim_ping_result_t;

typedef void (*sim_ping_result_fn)(
    void *context, const sim_ping_result_t *result
);

typedef struct sim_network sim_network_t;

sim_network_t *sim_network_create(
    const wlh_osal_ops_t *osal,
    sim_network_send_fn send,
    sim_ping_result_fn ping_result,
    void *context
);
void sim_network_destroy(sim_network_t *network);
int sim_network_link_up(sim_network_t *network, const uint8_t mac[6]);
void sim_network_link_down(sim_network_t *network);
int sim_network_input(
    sim_network_t *network, const uint8_t *frame, size_t size
);
int sim_network_ping(
    sim_network_t *network,
    uint32_t request_id,
    const char *hostname,
    uint32_t count,
    uint32_t timeout_ms
);

#endif
