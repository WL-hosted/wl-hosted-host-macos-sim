#include "sim.h"
#include "transport_usb.h"
#include "wlh/log.h"
#include "wlh/posix_osal.h"

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
    sim_executor_t tx_executor;
    wlh_posix_osal_t osal;
    wlh_host_t host;

    bool use_usb;
    sim_usb_config_t usb_config;
    sim_usb_transport_t *usb;
    const char *ssid;
    const char *credential;

    pthread_t rx_thread;
    pthread_mutex_t state_mutex;
    pthread_cond_t state_changed;

    atomic_bool running;
    atomic_uint fail_allocations;
    unsigned completions;
    bool scan_complete;
    bool connected;
    bool disconnected;
    bool ethernet_rx;
    bool device_info_done;
    wlh_host_result_t device_info_result;
    bool user_result_received;

    uint64_t started_ms;
    uint32_t monitor_interval_ms;
} app_t;

typedef struct tx_work {
    app_t *app;
    uint8_t *frame;
    size_t size;
    wlh_transport_tx_complete_fn completion;
    void *completion_context;
} tx_work_t;

typedef struct lifecycle_work {
    app_t *app;
    bool is_start;
    wlh_transport_lifecycle_complete_fn completion;
    void *completion_context;
} lifecycle_work_t;

static atomic_bool interrupted = false;

static uint64_t monotonic_ms(void) {
    struct timespec value;
    clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

static void signal_handler(int signal_number) {
    (void)signal_number;
    interrupted = true;
}
static void lifecycle_work_run(void *context) {
    lifecycle_work_t *work = context;
    work->completion(work->completion_context, 0);
    free(work);
}
static void usb_lifecycle_work_run(void *context) {
    lifecycle_work_t *work = context;
    int status = 0;
    if (work->is_start) {
        status = sim_usb_open(&work->app->usb, &work->app->usb_config);
    } else {
        sim_usb_close(work->app->usb);
        work->app->usb = NULL;
    }
    work->completion(work->completion_context, status);
    free(work);
}

static int submit_lifecycle(
    app_t *app,
    wlh_transport_lifecycle_complete_fn completion,
    void *completion_context,
    bool is_start
) {
    lifecycle_work_t *work = malloc(sizeof(*work));
    if (work == NULL)
        return -1;
    *work = (lifecycle_work_t){app, is_start, completion, completion_context};
    if (sim_executor_post(
            &app->tx_executor,
            app->use_usb ? usb_lifecycle_work_run : lifecycle_work_run,
            work
        ) != 0) {
        free(work);
        return -1;
    }
    return 0;
}

static int transport_start(
    void *context,
    wlh_transport_lifecycle_complete_fn completion,
    void *completion_context
) {
    return submit_lifecycle(context, completion, completion_context, true);
}
static int transport_stop(
    void *context,
    wlh_transport_lifecycle_complete_fn completion,
    void *completion_context
) {
    return submit_lifecycle(context, completion, completion_context, false);
}
static void tx_work_run(void *context) {
    tx_work_t *work = context;
    int status;
    if (work->app->use_usb) {
        status = sim_usb_write(work->app->usb, work->frame, work->size);
    } else {
        status = sim_ipc_write(
            &work->app->ipc, SIM_RECORD_WIRE_FRAME, work->frame, work->size
        );
    }
    work->completion(work->completion_context, work->frame, work->size, status);
    free(work);
}

static int transport_submit(
    void *context,
    uint8_t *frame,
    size_t size,
    wlh_transport_tx_complete_fn completion,
    void *completion_context
) {
    app_t *app = context;
    tx_work_t *work = malloc(sizeof(*work));
    if (work == NULL)
        return -1;
    *work = (tx_work_t){app, frame, size, completion, completion_context};
    if (sim_executor_post(&app->tx_executor, tx_work_run, work) != 0) {
        free(work);
        return -1;
    }
    return 0;
}
static uint8_t *buffer_alloc(void *context, size_t size) {
    app_t *app = context;
    unsigned remaining = atomic_load(&app->fail_allocations);
    while (remaining != 0u) {
        if (atomic_compare_exchange_weak(
                &app->fail_allocations, &remaining, remaining - 1u
            ))
            return NULL;
    }
    return malloc(size);
}
static void buffer_free(void *context, uint8_t *buffer) {
    (void)context;
    free(buffer);
}

static void completion(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const uint8_t *payload,
    size_t payload_size
) {
    app_t *app = context;
    (void)payload;
    (void)payload_size;
    pthread_mutex_lock(&app->state_mutex);
    app->completions++;
    pthread_cond_broadcast(&app->state_changed);
    pthread_mutex_unlock(&app->state_mutex);
    WLH_LOGI(
        "host-sim",
        "RPC completion result=%d domain=%u status=%d",
        result,
        domain,
        status
    );
}

static void device_info_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const wlh_host_device_info_t *info
) {
    app_t *app = context;
    WLH_LOGI(
        "host-sim",
        "device info result=%d domain=%u status=%d",
        result,
        domain,
        status
    );
    if (result == WLH_HOST_OK && info != NULL) {
        unsigned index;
        WLH_LOGI(
            "host-sim", "vendor=%s mcu_model=%s", info->vendor, info->mcu_model
        );
        WLH_LOGI("host-sim", "board_profile=%s", info->board_profile);
        fprintf(stdout, "host-sim: uid=");
        for (index = 0; index < info->uid_size; ++index)
            fprintf(stdout, "%02x", info->uid[index]);
        fputc('\n', stdout);
        fflush(stdout);
    }
    pthread_mutex_lock(&app->state_mutex);
    app->device_info_result = result;
    app->device_info_done = true;
    pthread_cond_broadcast(&app->state_changed);
    pthread_mutex_unlock(&app->state_mutex);
}

static void usb_on_frame(void *context, const uint8_t *frame, size_t size) {
    app_t *app = context;
    (void)wlh_host_on_frame(&app->host, frame, size);
}
static void usb_on_lost(void *context) {
    app_t *app = context;
    wlh_host_transport_lost(&app->host);
}

static void host_event(void *context, const wlh_host_event_t *event) {
    app_t *app = context;
    pthread_mutex_lock(&app->state_mutex);
    if (event->kind == WLH_HOST_EVENT_WIFI_SCAN_COMPLETED)
        app->scan_complete = true;
    if (event->kind == WLH_HOST_EVENT_WIFI_CONNECTED)
        app->connected = true;
    if (event->kind == WLH_HOST_EVENT_WIFI_DISCONNECTED)
        app->disconnected = true;
    if (event->kind == WLH_HOST_EVENT_ETHERNET_STA_RX)
        app->ethernet_rx = true;
    if (event->kind == WLH_HOST_EVENT_USER_MESSAGE_RESULT)
        app->user_result_received = true;
    pthread_cond_broadcast(&app->state_changed);
    pthread_mutex_unlock(&app->state_mutex);
    WLH_LOGI(
        "host-sim",
        "event kind=%d state=%d service=%u method=%u bytes=%zu",
        event->kind,
        event->state,
        event->service_id,
        event->method_id,
        event->payload_size
    );
}

static int send_protobuf(
    app_t *app,
    uint8_t kind,
    const pb_msgdesc_t *fields,
    const void *message,
    size_t maximum
) {
    uint8_t *payload = malloc(maximum);
    pb_ostream_t stream;
    int result;
    if (payload == NULL)
        return -1;
    stream = pb_ostream_from_buffer(payload, maximum);
    if (!pb_encode(&stream, fields, message)) {
        free(payload);
        return -1;
    }
    result = sim_ipc_write(&app->ipc, kind, payload, stream.bytes_written);
    free(payload);
    return result;
}

static void send_runtime(app_t *app) {
    wlh_host_diagnostics_t diagnostics;
    wlh_sim_v1_SimRuntimeInfo runtime = wlh_sim_v1_SimRuntimeInfo_init_zero;

    if (!app->ipc.sideband)
        return;

    wlh_host_get_diagnostics(&app->host, &diagnostics);

    runtime.role = wlh_sim_v1_SimRole_SIM_ROLE_HOST;
    runtime.link_state =
        diagnostics.state == WLH_HOST_STATE_READY
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
    memcpy(
        runtime.implementation,
        "wl-hosted-host-macos-sim",
        sizeof("wl-hosted-host-macos-sim")
    );
    memcpy(runtime.implementation_version, "0.1.0", sizeof("0.1.0"));

    (void)send_protobuf(
        app,
        SIM_RECORD_RUNTIME_INFO,
        wlh_sim_v1_SimRuntimeInfo_fields,
        &runtime,
        wlh_sim_v1_SimRuntimeInfo_size
    );
}

static void handle_fault(
    app_t *app, const uint8_t *payload, size_t payload_size
) {
    wlh_sim_v1_SimFaultRequest request = wlh_sim_v1_SimFaultRequest_init_zero;
    wlh_sim_v1_SimFaultResponse response =
        wlh_sim_v1_SimFaultResponse_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);

    response.status_code = WLH_STATUS_NOT_SUPPORTED;
    memcpy(
        response.detail,
        "not supported by host simulator",
        sizeof("not supported by host simulator")
    );

    if (!pb_decode(&stream, wlh_sim_v1_SimFaultRequest_fields, &request) ||
        request.request_id == 0u)
        return;
    response.request_id = request.request_id;

    switch (request.fault) {
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_HOST_RESET:
        wlh_host_transport_lost(&app->host);
        response.accepted = true;
        response.status_code = 0;
        memcpy(
            response.detail,
            "host transport reset",
            sizeof("host transport reset")
        );
        break;

    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_CLEAR_CREDIT:
    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_LIMIT_CREDIT:
        wlh_host_test_set_credit(&app->host, (uint8_t)request.channel, 0u);
        response.accepted = true;
        response.status_code = 0;
        memcpy(response.detail, "credit cleared", sizeof("credit cleared"));
        break;

    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_RPC_TIMEOUT:
        wlh_host_test_expire_all(&app->host);
        response.accepted = true;
        response.status_code = 0;
        memcpy(
            response.detail,
            "pending RPCs expired",
            sizeof("pending RPCs expired")
        );
        break;

    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_BUFFER_OOM:
        atomic_store(
            &app->fail_allocations, request.count == 0u ? 1u : request.count
        );
        response.accepted = true;
        response.status_code = 0;
        memcpy(response.detail, "buffer OOM armed", sizeof("buffer OOM armed"));
        break;

    case wlh_sim_v1_SimFaultKind_SIM_FAULT_KIND_QUEUE_STARVATION:
        usleep((useconds_t)(request.duration_ms > 60000u
                                ? 60000000u
                                : request.duration_ms * 1000u));
        response.accepted = true;
        response.status_code = 0;
        memcpy(
            response.detail, "RX worker delayed", sizeof("RX worker delayed")
        );
        break;

    default:
        break;
    }

    (void)send_protobuf(
        app,
        SIM_RECORD_FAULT_RESPONSE,
        wlh_sim_v1_SimFaultResponse_fields,
        &response,
        wlh_sim_v1_SimFaultResponse_size
    );
}

static void handle_wifi_command(
    app_t *app, const uint8_t *payload, size_t payload_size
) {
    wlh_sim_v1_SimWifiCommand message = wlh_sim_v1_SimWifiCommand_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
    const char *kind = NULL;
    wlh_host_result_t result = WLH_HOST_OK;

    if (!pb_decode(&stream, wlh_sim_v1_SimWifiCommand_fields, &message) ||
        message.command_id == 0u || message.which_command == 0u) {
        WLH_LOGW("host-sim", "wifi command ignored (invalid record)");
        return;
    }

    switch (message.which_command) {
    case wlh_sim_v1_SimWifiCommand_scan_tag: {
        size_t ssid_size = strnlen(
            message.command.scan.ssid, sizeof(message.command.scan.ssid)
        );
        wlh_wifi_scan_params_t params = {
            message.command.scan.scan_id,
            ssid_size == 0u ? NULL : (const uint8_t *)message.command.scan.ssid,
            ssid_size,
            message.command.scan.include_hidden,
            message.command.scan.max_results == 0u
                ? 8u
                : message.command.scan.max_results
        };
        kind = "scan";
        result = wlh_host_wifi_scan(&app->host, &params, completion, app);
        break;
    }

    case wlh_sim_v1_SimWifiCommand_connect_tag: {
        wlh_wifi_connect_params_t params = {
            (const uint8_t *)message.command.connect.ssid,
            strnlen(
                message.command.connect.ssid,
                sizeof(message.command.connect.ssid)
            ),
            (const uint8_t *)message.command.connect.credential,
            strnlen(
                message.command.connect.credential,
                sizeof(message.command.connect.credential)
            ),
            message.command.connect.security == 0u
                ? 4u
                : message.command.connect.security,
            message.command.connect.timeout_ms == 0u
                ? 3000u
                : message.command.connect.timeout_ms
        };
        kind = "connect";
        result = wlh_host_wifi_connect(&app->host, &params, completion, app);
        break;
    }

    case wlh_sim_v1_SimWifiCommand_disconnect_tag:
        kind = "disconnect";
        result = wlh_host_wifi_disconnect(&app->host, completion, app);
        break;

    case wlh_sim_v1_SimWifiCommand_start_ap_tag: {
        size_t credential_size = strnlen(
            message.command.start_ap.credential,
            sizeof(message.command.start_ap.credential)
        );
        wlh_wifi_start_ap_params_t params = {
            (const uint8_t *)message.command.start_ap.ssid,
            strnlen(
                message.command.start_ap.ssid,
                sizeof(message.command.start_ap.ssid)
            ),
            (const uint8_t *)message.command.start_ap.credential,
            credential_size,
            message.command.start_ap.security == 0u
                ? (credential_size == 0u ? 1u : 4u)
                : message.command.start_ap.security,
            message.command.start_ap.channel,
            message.command.start_ap.max_clients
        };
        kind = "start_ap";
        result = wlh_host_wifi_start_ap(&app->host, &params, completion, app);
        break;
    }

    case wlh_sim_v1_SimWifiCommand_stop_ap_tag:
        kind = "stop_ap";
        result = wlh_host_wifi_stop_ap(&app->host, completion, app);
        break;

    default:
        fprintf(
            stderr,
            "host-sim: wifi command id=%u unknown kind=%u\n",
            message.command_id,
            (unsigned)message.which_command
        );
        return;
    }

    WLH_LOGI("host-sim", "wifi command %s id=%u", kind, message.command_id);
    if (result != WLH_HOST_OK)
        WLH_LOGW(
            "host-sim",
            "wifi command %s id=%u rejected result=%d",
            kind,
            message.command_id,
            result
        );
}

static void *rx_main(void *context) {
    app_t *app = context;
    while (atomic_load(&app->running)) {
        uint8_t kind;
        uint8_t *payload = NULL;
        size_t payload_size = 0u;
        if (sim_ipc_read(&app->ipc, &kind, &payload, &payload_size) != 0)
            break;
        if (kind == SIM_RECORD_WIRE_FRAME) {
            (void)wlh_host_on_frame(&app->host, payload, payload_size);
        } else if (kind == SIM_RECORD_FAULT_REQUEST && app->ipc.sideband) {
            handle_fault(app, payload, payload_size);
        } else if (kind == SIM_RECORD_WIFI_COMMAND && app->ipc.sideband) {
            handle_wifi_command(app, payload, payload_size);
        }
        free(payload);
    }
    atomic_store(&app->running, false);
    pthread_mutex_lock(&app->state_mutex);
    pthread_cond_broadcast(&app->state_changed);
    pthread_mutex_unlock(&app->state_mutex);
    return NULL;
}

static struct timespec relative_duration_ms(uint64_t duration_ms) {
    struct timespec duration;
    duration.tv_sec = (time_t)(duration_ms / 1000u);
    duration.tv_nsec = (long)(duration_ms % 1000u) * 1000000L;
    return duration;
}

static bool wait_until(
    app_t *app, bool (*predicate)(app_t *), uint32_t timeout_ms
) {
    uint64_t deadline = monotonic_ms() + timeout_ms;
    uint64_t next_monitor = 0u;
    bool done = false;

    pthread_mutex_lock(&app->state_mutex);
    while (atomic_load(&app->running) && !atomic_load(&interrupted)) {
        uint64_t now = monotonic_ms();
        uint64_t wake_at;
        struct timespec native_duration;
        done = predicate(app);
        if (done || now >= deadline)
            break;
        if (now >= next_monitor) {
            pthread_mutex_unlock(&app->state_mutex);
            send_runtime(app);
            now = monotonic_ms();
            next_monitor = now + app->monitor_interval_ms;
            pthread_mutex_lock(&app->state_mutex);
        }
        wake_at = deadline < next_monitor ? deadline : next_monitor;
        now = monotonic_ms();
        native_duration =
            relative_duration_ms(wake_at > now ? wake_at - now : 0u);
        (void)pthread_cond_timedwait_relative_np(
            &app->state_changed, &app->state_mutex, &native_duration
        );
    }
    pthread_mutex_unlock(&app->state_mutex);
    return done;
}

static bool ready(app_t *app) {
    wlh_host_diagnostics_t diagnostics;
    wlh_host_get_diagnostics(&app->host, &diagnostics);
    return diagnostics.state == WLH_HOST_STATE_READY;
}
static bool not_ready(app_t *app) {
    return !ready(app);
}
static bool one_completion(app_t *app) {
    return app->completions >= 1u;
}
static bool scan_complete(app_t *app) {
    return app->scan_complete;
}
static bool connected(app_t *app) {
    return app->connected;
}
static bool ethernet_rx(app_t *app) {
    return app->ethernet_rx;
}
static bool disconnected(app_t *app) {
    return app->disconnected;
}
static bool device_info_ready(app_t *app) {
    return app->device_info_done;
}
static bool user_result_arrived(app_t *app) {
    return app->user_result_received;
}

static int run_managed(app_t *app) {
    while (atomic_load(&app->running) && !atomic_load(&interrupted)) {
        if (!wait_until(app, ready, 30000u))
            break;
        app->completions = 0u;
        (void)wlh_host_wifi_initialize(&app->host, completion, app);
        /* Repeat INITIALIZE is idempotent; tolerate a missing completion. */
        if (!wait_until(app, one_completion, 3000u))
            WLH_LOGW("host-sim", "managed initialize not confirmed");
        (void)wait_until(app, not_ready, UINT32_MAX);
    }
    return atomic_load(&app->running) && !atomic_load(&interrupted) ? -1 : 0;
}

static int run_scenario(app_t *app, const char *scenario) {
    wlh_wifi_scan_params_t scan = {1u, NULL, 0u, true, 8u};
    static const uint8_t default_ssid[] = "WPA2Net";
    static const uint8_t default_credential[] = "password123";
    static const uint8_t user_payload[] = "hello-coproc";
    const uint8_t *ssid = default_ssid;
    size_t ssid_size = sizeof(default_ssid) - 1u;
    const uint8_t *credential = default_credential;
    size_t credential_size = sizeof(default_credential) - 1u;
    wlh_wifi_connect_params_t connect;
    uint8_t ethernet[60] = {0x02, 0, 0, 0, 0, 2, 0x02, 0, 0, 0, 0, 1};

    if (app->ssid != NULL) {
        ssid = (const uint8_t *)app->ssid;
        ssid_size = strlen(app->ssid);
    }
    if (app->credential != NULL) {
        credential = (const uint8_t *)app->credential;
        credential_size = strlen(app->credential);
    }
    connect = (wlh_wifi_connect_params_t){
        ssid, ssid_size, credential, credential_size, 4u, 3000u
    };

    /* Managed mode owns its (longer) READY wait; skip the scenario gate. */
    if (strcmp(scenario, "managed") == 0)
        return run_managed(app);

    if (!wait_until(app, ready, 5000u))
        return -1;
    if (strcmp(scenario, "smoke") == 0)
        return 0;
    if (strcmp(scenario, "recovery") == 0) {
        wlh_host_transport_lost(&app->host);
        if (!wait_until(app, not_ready, 2000u))
            return -1;
        return wait_until(app, ready, 5000u) ? 0 : -1;
    }
    if (strcmp(scenario, "services") == 0) {
        (void)wlh_host_get_device_info(&app->host, device_info_completion, app);
        if (!wait_until(app, device_info_ready, 3000u))
            return -1;
        if (app->device_info_result != WLH_HOST_OK)
            return -1;
        app->completions = 0u;
        (void)wlh_host_user_message_send(
            &app->host,
            1u,
            1u,
            1u /* EXPECT_RESULT */,
            user_payload,
            sizeof(user_payload) - 1u,
            completion,
            app
        );
        if (!wait_until(app, one_completion, 3000u))
            return -1;
        /* A RESULT event is optional; give it a short window. */
        (void)wait_until(app, user_result_arrived, 1500u);
        return 0;
    }

    app->completions = 0u;
    (void)wlh_host_wifi_initialize(&app->host, completion, app);
    if (!wait_until(app, one_completion, 3000u))
        return -1;

    (void)wlh_host_wifi_scan(&app->host, &scan, completion, app);
    if (!wait_until(app, scan_complete, 3000u))
        return -1;
    if (strcmp(scenario, "scan") == 0)
        return 0;

    (void)wlh_host_wifi_connect(&app->host, &connect, completion, app);
    if (!wait_until(app, connected, 4000u))
        return -1;

    /* Ethernet echo is a mock-coprocessor behavior; a real device forwards
       the frame to the AP instead, so USB mode skips the echo check. */
    if (!app->use_usb) {
        (void)wlh_host_ethernet_sta_send(
            &app->host, ethernet, sizeof(ethernet)
        );
        if (!wait_until(app, ethernet_rx, 3000u))
            return -1;
    }

    (void)wlh_host_wifi_disconnect(&app->host, completion, app);
    return wait_until(app, disconnected, 3000u) ? 0 : -1;
}

static void usage(const char *program) {
    fprintf(
        stderr,
        "usage: %s --ipc connect:PATH|fd:N | --usb VID:PID [--scenario "
        "smoke|scan|connect|recovery|services|managed] "
        "[--monitor-interval-ms N] [--rpc-timeout-ms N] "
        "[--ssid SSID] [--credential CREDENTIAL]\n",
        program
    );
}

static bool parse_usb_ids(
    const char *text, uint16_t *vendor, uint16_t *product
) {
    char *separator;
    unsigned long vendor_value, product_value;
    if (text == NULL)
        return false;
    vendor_value = strtoul(text, &separator, 16);
    if (separator == text || *separator != ':' || vendor_value > 0xffffu)
        return false;
    product_value = strtoul(separator + 1, NULL, 16);
    if (product_value > 0xffffu)
        return false;
    *vendor = (uint16_t)vendor_value;
    *product = (uint16_t)product_value;
    return true;
}

int main(int argc, char **argv) {
    app_t app;
    const char *endpoint = NULL;
    const char *scenario = "connect";
    uint32_t rpc_timeout_ms = 3000u;
    wlh_host_config_t config;
    int index;
    int result;

    memset(&app, 0, sizeof(app));
    app.monitor_interval_ms = 1000u;
    app.usb_config = (sim_usb_config_t){0x303au,
                                        0x8201u,
                                        0u,
                                        0x01u,
                                        0x81u,
                                        4096u,
                                        10000u,
                                        usb_on_frame,
                                        usb_on_lost,
                                        &app};

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--ipc") == 0 && ++index < argc)
            endpoint = argv[index];
        else if (strcmp(argv[index], "--usb") == 0 && ++index < argc) {
            app.use_usb = true;
            if (!parse_usb_ids(
                    argv[index],
                    &app.usb_config.vendor_id,
                    &app.usb_config.product_id
                )) {
                usage(argv[0]);
                return 2;
            }
        } else if (strcmp(argv[index], "--ssid") == 0 && ++index < argc)
            app.ssid = argv[index];
        else if (strcmp(argv[index], "--credential") == 0 && ++index < argc)
            app.credential = argv[index];
        else if (strcmp(argv[index], "--scenario") == 0 && ++index < argc)
            scenario = argv[index];
        else if (strcmp(argv[index], "--monitor-interval-ms") == 0 &&
                 ++index < argc)
            app.monitor_interval_ms = (uint32_t)strtoul(argv[index], NULL, 10);
        else if (strcmp(argv[index], "--rpc-timeout-ms") == 0 && ++index < argc)
            rpc_timeout_ms = (uint32_t)strtoul(argv[index], NULL, 10);
        else {
            usage(argv[0]);
            return 2;
        }
    }
    if ((endpoint == NULL) == !app.use_usb || app.monitor_interval_ms == 0u) {
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    /* Managed sessions are long-lived: a Manager disconnect must surface as
     * EPIPE from write(), never as a fatal SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);

    WLH_LOG_INIT();
    WLH_LOGI(
        "host-sim",
        "starting scenario=%s endpoint=%s usb=%s rpc_timeout_ms=%u",
        scenario,
        app.use_usb ? "usb" : endpoint,
        app.use_usb ? "yes" : "no",
        rpc_timeout_ms
    );

    if (pthread_mutex_init(&app.state_mutex, NULL) != 0 ||
        pthread_cond_init(&app.state_changed, NULL) != 0 ||
        sim_executor_start(&app.executor) != 0 ||
        sim_executor_start(&app.tx_executor) != 0 ||
        (!app.use_usb && sim_ipc_open(&app.ipc, endpoint) != 0)) {
        WLH_LOGE("host-sim", "initialization failed");
        return 1;
    }

    memset(&config, 0, sizeof(config));
    wlh_posix_osal_init(&app.osal);

    // clang-format off
    config.transport = (wlh_transport_ops_t){
        &app, transport_start, transport_stop, transport_submit};
    config.buffers = (wlh_buffer_ops_t){
        &app, buffer_alloc, buffer_free};
    config.osal = wlh_posix_osal_ops(&app.osal);
    config.executor = (wlh_executor_ops_t){
        &app.executor, sim_executor_post};
    // clang-format on

    config.on_event = host_event;
    config.event_context = &app;

    config.max_frame_size = 4096u;
    config.rpc_timeout_ms = rpc_timeout_ms;
    config.heartbeat_timeout_ms = 5000u;
    config.max_pending_rpc = 8u;
    config.core_queue_depth = 16u;
    config.stop_timeout_ms = 3000u;

    if (wlh_host_init(&app.host, &config) != WLH_HOST_OK)
        return 1;
    app.started_ms = monotonic_ms();
    atomic_store(&app.running, true);

    if (!app.use_usb &&
        pthread_create(&app.rx_thread, NULL, rx_main, &app) != 0)
        return 1;
    result = wlh_host_start(&app.host);

    if (result == WLH_HOST_OK)
        result = run_scenario(&app, scenario);
    send_runtime(&app);

    (void)wlh_host_stop(&app.host);
    sim_executor_stop(&app.tx_executor);
    atomic_store(&app.running, false);
    if (!app.use_usb) {
        sim_ipc_close(&app.ipc);
        pthread_join(app.rx_thread, NULL);
    }
    sim_executor_stop(&app.executor);
    pthread_cond_destroy(&app.state_changed);
    pthread_mutex_destroy(&app.state_mutex);

    return result == 0 ? 0 : 1;
}
