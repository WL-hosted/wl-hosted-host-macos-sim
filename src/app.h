#ifndef WLH_HOST_SIM_APP_H
#define WLH_HOST_SIM_APP_H

#include "network.h"
#include "sim.h"
#include "transport_usb.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "wlh/host.h"
#include "wlh/posix_osal.h"
#include <wifi.pb.h>

/* REPL session state. The line hand-off slot and command-progress flags are
 * protected by app_t.state_mutex / signalled through app_t.state_changed. */
typedef struct app_repl {
    bool active;

    char *pending_line;
    bool line_ready;
    bool stdin_eof;
    pthread_t reader;
    bool reader_started;
    atomic_bool reader_stop;

    bool wifi_initialized;
    bool op_done;
    wlh_host_result_t op_result;
    uint16_t op_domain;
    int16_t op_status;
    bool device_info_ok;
    wlh_host_device_info_t device_info;
    uint32_t ping_request_id;
    bool ping_done;
    sim_ping_result_t ping_result;
} app_repl_t;

typedef struct wlh_iperf_controller wlh_iperf_controller_t;

typedef struct app {
    sim_ipc_t ipc;
    sim_executor_t executor;
    sim_executor_t tx_executor;
    wlh_posix_osal_t osal;
    wlh_host_t host;
    sim_network_t *network;

    bool use_usb;
    sim_usb_config_t usb_config;
    sim_usb_transport_t *usb;
    uint64_t ota_image_size;

    /* Written only by tx_executor.  These counters make USB batching
     * observable without adding synchronization to the data path. */
    uint64_t usb_tx_batch_count;
    uint64_t usb_tx_frame_count;
    uint64_t usb_tx_byte_count;
    uint64_t usb_tx_last_report_ms;
    unsigned usb_tx_max_batch_frames;
    size_t usb_tx_max_batch_bytes;

    pthread_t rx_thread;
    pthread_mutex_t state_mutex;
    pthread_cond_t state_changed;

    atomic_bool running;
    atomic_uint fail_allocations;
    unsigned completions;
    wlh_host_result_t last_completion_result;
    bool ota_begin_done;
    wlh_host_ota_begin_response_t ota_begin;
    bool ota_query_done;
    wlh_host_ota_query_response_t ota_query;
    unsigned ota_credit_events;
    unsigned ota_credit_seen;
    bool ota_activate_done;
    wlh_host_result_t ota_activate_result;
    bool ota_left_ready;
    bool scan_complete;
    bool connected;
    bool disconnected;
    bool ethernet_rx;
    bool user_result_received;
    /* Wired-ETH mode. eth_mode is set once the READY-time ETH GET_INFO probe
     * succeeds against the peer firmware; it is atomic because network_send
     * reads it from the lwIP tcpip thread. eth_link_up and eth_mac are
     * guarded by state_mutex. */
    atomic_bool eth_mode;
    bool eth_link_up;
    uint8_t eth_mac[6];

    uint64_t started_ms;
    uint32_t monitor_interval_ms;

    wlh_osal_ops_t osal_ops;

    app_repl_t repl;
    wlh_iperf_controller_t *iperf;
} app_t;

uint64_t wlh_app_monotonic_ms(void);
struct timespec wlh_app_relative_duration_ms(uint64_t duration_ms);
bool wlh_app_interrupted(void);

/* Waits under state_mutex until the predicate holds, the timeout elapses, the
 * transport dies, or a termination signal arrives; sends periodic runtime
 * info while waiting. Returns the final predicate value. */
bool wlh_app_wait_until(
    app_t *app, bool (*predicate)(app_t *), uint32_t timeout_ms
);

/* Runs the full OTA flow (query/abort-stale, begin, stream, finalize,
 * activate, reboot wait, version verify). Blocks the calling thread.
 * expected_version NULL skips verification; timeout_ms 0 selects 30000.
 * On failure *fail_stage (optional) names the failing step. */
int wlh_app_run_ota(
    app_t *app,
    const char *image_path,
    const char *expected_version,
    uint32_t timeout_ms,
    const char **fail_stage
);

const char *wlh_app_wifi_security_name(wlh_protocol_v1_WifiSecurity security);

typedef void (*wlh_app_scan_network_fn)(
    void *context, uint32_t scan_id, const wlh_protocol_v1_WifiNetwork *network
);
/* Decodes a WLH_HOST_EVENT_WIFI_SCAN_RESULT payload and invokes the callback
 * once per network. Returns -1 when the payload cannot be decoded. */
int wlh_app_for_each_scan_network(
    const wlh_host_event_t *event,
    wlh_app_scan_network_fn callback,
    void *context
);

#endif
