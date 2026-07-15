#include "sim.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sim_sideband.pb.h"
#include "wlh/host.h"
#include <pb_decode.h>
#include <pb_encode.h>

typedef struct app {
    sim_ipc_t ipc;
    sim_executor_t executor;
    wlh_host_t host;
    pthread_t rx_thread;
    pthread_mutex_t core_mutex;
    pthread_mutex_t state_mutex;
    atomic_bool running;
    atomic_uint fail_allocations;
    unsigned completions;
    bool scan_complete;
    bool connected;
    bool disconnected;
    bool ethernet_rx;
    uint64_t started_ms;
    uint32_t monitor_interval_ms;
} app_t;

static atomic_bool interrupted = false;

static uint64_t monotonic_ms(void)
{
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

static void signal_handler(int signal_number) { (void)signal_number; interrupted = true; }
static int transport_start(void *context) { (void)context; return 0; }
static int transport_stop(void *context) { (void)context; return 0; }
static int transport_send(void *context, const uint8_t *frame, size_t size)
{ return sim_ipc_write(&((app_t *)context)->ipc, SIM_RECORD_WIRE_FRAME, frame, size); }
static uint8_t *buffer_alloc(void *context, size_t size)
{
    app_t *app = context;
    unsigned remaining = atomic_load(&app->fail_allocations);
    while (remaining != 0u) {
        if (atomic_compare_exchange_weak(&app->fail_allocations, &remaining, remaining - 1u))
            return NULL;
    }
    return malloc(size);
}
static void buffer_free(void *context, uint8_t *buffer) { (void)context; free(buffer); }
static uint64_t host_now(void *context) { (void)context; return monotonic_ms(); }

static void completion(void *context, wlh_host_result_t result, uint16_t domain,
                       int16_t status, const uint8_t *payload, size_t payload_size)
{
    app_t *app = context;
    (void)payload; (void)payload_size;
    pthread_mutex_lock(&app->state_mutex);
    app->completions++;
    pthread_mutex_unlock(&app->state_mutex);
    fprintf(stderr, "host-sim: RPC completion result=%d domain=%u status=%d\n",
            result, domain, status);
}

static void host_event(void *context, const wlh_host_event_t *event)
{
    app_t *app = context;
    pthread_mutex_lock(&app->state_mutex);
    if (event->kind == WLH_HOST_EVENT_WIFI_SCAN_COMPLETED) app->scan_complete = true;
    if (event->kind == WLH_HOST_EVENT_WIFI_CONNECTED) app->connected = true;
    if (event->kind == WLH_HOST_EVENT_WIFI_DISCONNECTED) app->disconnected = true;
    if (event->kind == WLH_HOST_EVENT_ETHERNET_STA_RX) app->ethernet_rx = true;
    pthread_mutex_unlock(&app->state_mutex);
    fprintf(stderr, "host-sim: event=%d state=%d service=%u method=%u bytes=%zu\n",
            event->kind, event->state, event->service_id, event->method_id, event->payload_size);
}

static int send_protobuf(app_t *app, uint8_t kind, const pb_msgdesc_t *fields, const void *message,
                         size_t maximum)
{
    uint8_t *payload = malloc(maximum);
    pb_ostream_t stream;
    int result;
    if (payload == NULL) return -1;
    stream = pb_ostream_from_buffer(payload, maximum);
    if (!pb_encode(&stream, fields, message)) { free(payload); return -1; }
    result = sim_ipc_write(&app->ipc, kind, payload, stream.bytes_written);
    free(payload);
    return result;
}

static void send_runtime(app_t *app)
{
    wlh_host_diagnostics_t diagnostics;
    wlh_sim_v1_SimRuntimeInfo runtime = wlh_sim_v1_SimRuntimeInfo_init_zero;
    if (!app->ipc.sideband) return;
    pthread_mutex_lock(&app->core_mutex);
    wlh_host_get_diagnostics(&app->host, &diagnostics);
    runtime.role = wlh_sim_v1_SimRole_SIM_ROLE_HOST;
    runtime.link_state = diagnostics.state == WLH_HOST_STATE_READY
                             ? wlh_sim_v1_SimLinkState_SIM_LINK_STATE_UP
                             : diagnostics.state == WLH_HOST_STATE_RECOVERING
                                   ? wlh_sim_v1_SimLinkState_SIM_LINK_STATE_RECOVERING
                                   : wlh_sim_v1_SimLinkState_SIM_LINK_STATE_NEGOTIATING;
    runtime.session_id = diagnostics.session_id;
    runtime.uptime_ms = monotonic_ms() - app->started_ms;
    runtime.tx_frames = diagnostics.tx_frames;
    runtime.rx_frames = diagnostics.rx_frames;
    runtime.dropped_frames = diagnostics.checksum_errors;
    runtime.free_buffers = 64u;
    memcpy(runtime.implementation, "wl-hosted-host-macos-sim",
           sizeof("wl-hosted-host-macos-sim"));
    memcpy(runtime.implementation_version, "0.1.0", sizeof("0.1.0"));
    pthread_mutex_unlock(&app->core_mutex);
    (void)send_protobuf(app, SIM_RECORD_RUNTIME_INFO, wlh_sim_v1_SimRuntimeInfo_fields, &runtime,
                        wlh_sim_v1_SimRuntimeInfo_size);
}

static void handle_fault(app_t *app, const uint8_t *payload, size_t payload_size)
{
    wlh_sim_v1_SimFaultRequest request = wlh_sim_v1_SimFaultRequest_init_zero;
    wlh_sim_v1_SimFaultResponse response = wlh_sim_v1_SimFaultResponse_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
    response.status_code = WLH_STATUS_NOT_SUPPORTED;
    memcpy(response.detail, "not supported by host simulator",
           sizeof("not supported by host simulator"));
    if (!pb_decode(&stream, wlh_sim_v1_SimFaultRequest_fields, &request) ||
        request.request_id == 0u) return;
    response.request_id = request.request_id;
    pthread_mutex_lock(&app->core_mutex);
    switch (request.fault) {
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_HOST_RESET:
        wlh_host_transport_lost(&app->host); response.accepted = true; response.status_code = 0;
        memcpy(response.detail, "host transport reset", sizeof("host transport reset")); break;
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_CLEAR_CREDIT:
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_LIMIT_CREDIT:
        wlh_host_test_set_credit(&app->host, (uint8_t)request.channel, 0u);
        response.accepted = true; response.status_code = 0;
        memcpy(response.detail, "credit cleared", sizeof("credit cleared")); break;
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_RPC_TIMEOUT:
        wlh_host_test_expire_all(&app->host); response.accepted = true; response.status_code = 0;
        memcpy(response.detail, "pending RPCs expired", sizeof("pending RPCs expired")); break;
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_BUFFER_OOM:
        atomic_store(&app->fail_allocations, request.count == 0u ? 1u : request.count);
        response.accepted = true; response.status_code = 0;
        memcpy(response.detail, "buffer OOM armed", sizeof("buffer OOM armed")); break;
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_QUEUE_STARVATION:
        usleep((useconds_t)(request.duration_ms > 60000u ? 60000000u
                                                         : request.duration_ms * 1000u));
        response.accepted = true; response.status_code = 0;
        memcpy(response.detail, "RX worker delayed", sizeof("RX worker delayed")); break;
    default: break;
    }
    pthread_mutex_unlock(&app->core_mutex);
    (void)send_protobuf(app, SIM_RECORD_FAULT_RESPONSE, wlh_sim_v1_SimFaultResponse_fields,
                        &response, wlh_sim_v1_SimFaultResponse_size);
}

static void *rx_main(void *context)
{
    app_t *app = context;
    while (atomic_load(&app->running)) {
        uint8_t kind;
        uint8_t *payload = NULL;
        size_t payload_size = 0u;
        if (sim_ipc_read(&app->ipc, &kind, &payload, &payload_size) != 0) break;
        if (kind == SIM_RECORD_WIRE_FRAME) {
            pthread_mutex_lock(&app->core_mutex);
            (void)wlh_host_on_frame(&app->host, payload, payload_size);
            pthread_mutex_unlock(&app->core_mutex);
        } else if (kind == SIM_RECORD_FAULT_REQUEST && app->ipc.sideband) {
            handle_fault(app, payload, payload_size);
        }
        free(payload);
    }
    atomic_store(&app->running, false);
    return NULL;
}

static bool wait_until(app_t *app, bool (*predicate)(app_t *), uint32_t timeout_ms)
{
    uint64_t deadline = monotonic_ms() + timeout_ms;
    uint64_t next_monitor = 0u;
    while (atomic_load(&app->running) && !atomic_load(&interrupted) && monotonic_ms() < deadline) {
        bool done;
        pthread_mutex_lock(&app->core_mutex);
        (void)wlh_host_poll(&app->host);
        pthread_mutex_unlock(&app->core_mutex);
        pthread_mutex_lock(&app->state_mutex);
        done = predicate(app);
        pthread_mutex_unlock(&app->state_mutex);
        if (done) return true;
        if (monotonic_ms() >= next_monitor) {
            send_runtime(app);
            next_monitor = monotonic_ms() + app->monitor_interval_ms;
        }
        usleep(10000u);
    }
    return false;
}

static bool ready(app_t *app) { return app->host.state == WLH_HOST_STATE_READY; }
static bool one_completion(app_t *app) { return app->completions >= 1u; }
static bool scan_complete(app_t *app) { return app->scan_complete; }
static bool connected(app_t *app) { return app->connected; }
static bool ethernet_rx(app_t *app) { return app->ethernet_rx; }
static bool disconnected(app_t *app) { return app->disconnected; }

static int run_scenario(app_t *app, const char *scenario)
{
    wlh_wifi_scan_params_t scan = {1u, NULL, 0u, true, 8u};
    static const uint8_t ssid[] = "Lab-WPA2";
    static const uint8_t credential[] = "password";
    wlh_wifi_connect_params_t connect = {ssid, sizeof(ssid) - 1u, credential,
                                         sizeof(credential) - 1u, 4u, 3000u};
    uint8_t ethernet[60] = {0x02, 0, 0, 0, 0, 2, 0x02, 0, 0, 0, 0, 1};
    if (!wait_until(app, ready, 5000u)) return -1;
    if (strcmp(scenario, "smoke") == 0) return 0;
    pthread_mutex_lock(&app->core_mutex);
    app->completions = 0u;
    (void)wlh_host_wifi_initialize(&app->host, completion, app);
    pthread_mutex_unlock(&app->core_mutex);
    if (!wait_until(app, one_completion, 3000u)) return -1;
    pthread_mutex_lock(&app->core_mutex);
    (void)wlh_host_wifi_scan(&app->host, &scan, completion, app);
    pthread_mutex_unlock(&app->core_mutex);
    if (!wait_until(app, scan_complete, 3000u)) return -1;
    pthread_mutex_lock(&app->core_mutex);
    (void)wlh_host_wifi_connect(&app->host, &connect, completion, app);
    pthread_mutex_unlock(&app->core_mutex);
    if (!wait_until(app, connected, 4000u)) return -1;
    pthread_mutex_lock(&app->core_mutex);
    (void)wlh_host_ethernet_sta_send(&app->host, ethernet, sizeof(ethernet));
    pthread_mutex_unlock(&app->core_mutex);
    if (!wait_until(app, ethernet_rx, 3000u)) return -1;
    pthread_mutex_lock(&app->core_mutex);
    (void)wlh_host_wifi_disconnect(&app->host, completion, app);
    pthread_mutex_unlock(&app->core_mutex);
    return wait_until(app, disconnected, 3000u) ? 0 : -1;
}

static void usage(const char *program)
{
    fprintf(stderr, "usage: %s --ipc connect:PATH|fd:N [--scenario smoke|scan|connect|recovery] "
                    "[--monitor-interval-ms N] [--rpc-timeout-ms N]\n", program);
}

int main(int argc, char **argv)
{
    app_t app;
    const char *endpoint = NULL;
    const char *scenario = "connect";
    uint32_t rpc_timeout_ms = 3000u;
    wlh_host_config_t config;
    int index;
    int result;
    memset(&app, 0, sizeof(app));
    app.monitor_interval_ms = 1000u;
    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--ipc") == 0 && ++index < argc) endpoint = argv[index];
        else if (strcmp(argv[index], "--scenario") == 0 && ++index < argc) scenario = argv[index];
        else if (strcmp(argv[index], "--monitor-interval-ms") == 0 && ++index < argc)
            app.monitor_interval_ms = (uint32_t)strtoul(argv[index], NULL, 10);
        else if (strcmp(argv[index], "--rpc-timeout-ms") == 0 && ++index < argc)
            rpc_timeout_ms = (uint32_t)strtoul(argv[index], NULL, 10);
        else { usage(argv[0]); return 2; }
    }
    if (endpoint == NULL || app.monitor_interval_ms == 0u) { usage(argv[0]); return 2; }
    signal(SIGINT, signal_handler); signal(SIGTERM, signal_handler);
    if (pthread_mutex_init(&app.core_mutex, NULL) != 0 ||
        pthread_mutex_init(&app.state_mutex, NULL) != 0 || sim_executor_start(&app.executor) != 0 ||
        sim_ipc_open(&app.ipc, endpoint) != 0) {
        fprintf(stderr, "host-sim: initialization failed\n"); return 1;
    }
    memset(&config, 0, sizeof(config));
    config.transport = (wlh_transport_ops_t){&app, transport_start, transport_stop, transport_send};
    config.buffers = (wlh_buffer_ops_t){&app, buffer_alloc, buffer_free};
    config.osal = (wlh_osal_ops_t){&app, host_now};
    config.executor = (wlh_executor_ops_t){&app.executor, sim_executor_post};
    config.on_event = host_event; config.event_context = &app;
    config.max_frame_size = 4096u; config.rpc_timeout_ms = rpc_timeout_ms;
    config.heartbeat_interval_ms = 1000u; config.heartbeat_timeout_ms = 5000u;
    config.max_pending_rpc = 8u;
    if (wlh_host_init(&app.host, &config) != WLH_HOST_OK) return 1;
    app.started_ms = monotonic_ms();
    atomic_store(&app.running, true);
    if (pthread_create(&app.rx_thread, NULL, rx_main, &app) != 0) return 1;
    pthread_mutex_lock(&app.core_mutex);
    result = wlh_host_start(&app.host);
    pthread_mutex_unlock(&app.core_mutex);
    if (result == WLH_HOST_OK) result = run_scenario(&app, scenario);
    send_runtime(&app);
    atomic_store(&app.running, false);
    sim_ipc_close(&app.ipc);
    pthread_join(app.rx_thread, NULL);
    pthread_mutex_lock(&app.core_mutex);
    (void)wlh_host_stop(&app.host);
    pthread_mutex_unlock(&app.core_mutex);
    sim_executor_stop(&app.executor);
    pthread_mutex_destroy(&app.state_mutex); pthread_mutex_destroy(&app.core_mutex);
    return result == 0 ? 0 : 1;
}
