#ifndef WLH_HOST_SIM_IPERF_CONTROLLER_H
#define WLH_HOST_SIM_IPERF_CONTROLLER_H

#include "app.h"

#include <stdbool.h>
#include <stdint.h>

typedef enum wlh_iperf_protocol {
    WLH_IPERF_TCP,
    WLH_IPERF_UDP
} wlh_iperf_protocol_t;
typedef enum wlh_iperf_role {
    WLH_IPERF_CLIENT,
    WLH_IPERF_SERVER
} wlh_iperf_role_t;

typedef struct wlh_iperf_request {
    wlh_iperf_protocol_t protocol;
    wlh_iperf_role_t role;
    const char *peer; /* required for client, IPv4 literal */
    uint32_t duration_sec;
    uint32_t target_mbps;
} wlh_iperf_request_t;

wlh_iperf_controller_t *wlh_iperf_controller_create(app_t *app);
void wlh_iperf_controller_destroy(wlh_iperf_controller_t *controller);
int wlh_iperf_start(
    wlh_iperf_controller_t *controller, const wlh_iperf_request_t *request
);
void wlh_iperf_cancel(wlh_iperf_controller_t *controller, const char *detail);

#endif
