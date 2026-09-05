#include "app.h"
#include "iperf_controller.h"
#include "network.h"
#include "repl.h"
#include "sim.h"
#include "transport_usb.h"
#include "wlh/log.h"
#include "wlh/posix_osal.h"

#include <CommonCrypto/CommonDigest.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "sim_sideband.pb.h"
#include "wlh/host.h"
#include <ota.pb.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <wifi.pb.h>

#include "ble/hci_transport.h"

typedef struct tx_work {
    app_t *app;
    uint8_t *frame;
    size_t size;
    wlh_transport_tx_complete_fn completion;
    void *completion_context;
} tx_work_t;

#define USB_TX_BATCH_MAX_FRAMES 4u
#define USB_TX_BATCH_MAX_BYTES (16u * 1024u)
#define USB_TX_STATS_INTERVAL_MS 30000u

typedef struct lifecycle_work {
    app_t *app;
    bool is_start;
    wlh_transport_lifecycle_complete_fn completion;
    void *completion_context;
} lifecycle_work_t;

static atomic_bool interrupted = false;

static int send_protobuf(
    app_t *app,
    uint8_t kind,
    const pb_msgdesc_t *fields,
    const void *message,
    size_t maximum
);
static void tx_work_run(void *context);

uint64_t wlh_app_monotonic_ms(void) {
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
static tx_work_t *take_adjacent_usb_tx(app_t *app, size_t available) {
    sim_executor_t *executor = &app->tx_executor;
    tx_work_t *work = NULL;

    pthread_mutex_lock(&executor->mutex);
    if (executor->count != 0u) {
        sim_task_t *task = &executor->queue[executor->head];
        tx_work_t *candidate = task->context;

        /* Never cross a lifecycle or other executor task.  Transport order is
         * part of the Core contract, so only consecutive TX submissions may
         * share a USB transfer. */
        if (task->function == tx_work_run && candidate->size <= available) {
            work = candidate;
            executor->head = (executor->head + 1u) % 64u;
            executor->count--;
        }
    }
    pthread_mutex_unlock(&executor->mutex);
    return work;
}

static void complete_tx_work(tx_work_t *work, int status) {
    work->completion(work->completion_context, work->frame, work->size, status);
    free(work);
}

static void report_usb_tx_stats(
    app_t *app, size_t batch_frames, size_t batch_bytes
) {
    uint64_t now = wlh_app_monotonic_ms();

    app->usb_tx_batch_count++;
    app->usb_tx_frame_count += batch_frames;
    app->usb_tx_byte_count += batch_bytes;
    if (batch_frames > app->usb_tx_max_batch_frames)
        app->usb_tx_max_batch_frames = (unsigned)batch_frames;
    if (batch_bytes > app->usb_tx_max_batch_bytes)
        app->usb_tx_max_batch_bytes = batch_bytes;
    if (now - app->usb_tx_last_report_ms < USB_TX_STATS_INTERVAL_MS)
        return;
    app->usb_tx_last_report_ms = now;
    WLH_LOGI(
        "host-sim",
        "USB TX stats: batches=%llu frames=%llu bytes=%llu max=%u/%zu",
        (unsigned long long)app->usb_tx_batch_count,
        (unsigned long long)app->usb_tx_frame_count,
        (unsigned long long)app->usb_tx_byte_count,
        app->usb_tx_max_batch_frames,
        app->usb_tx_max_batch_bytes
    );
}

static void tx_work_run(void *context) {
    tx_work_t *works[USB_TX_BATCH_MAX_FRAMES];
    uint8_t batch[USB_TX_BATCH_MAX_BYTES];
    tx_work_t *work = context;
    size_t count = 1u;
    size_t size = work->size;
    int status;

    works[0] = work;
    if (work->app->use_usb) {
        while (count < USB_TX_BATCH_MAX_FRAMES && size < sizeof(batch)) {
            tx_work_t *next =
                take_adjacent_usb_tx(work->app, sizeof(batch) - size);
            if (next == NULL)
                break;
            works[count++] = next;
            size += next->size;
        }

        if (count == 1u) {
            status = sim_usb_write(work->app->usb, work->frame, work->size);
        } else {
            size_t cursor = 0u;
            for (size_t index = 0u; index < count; ++index) {
                memcpy(batch + cursor, works[index]->frame, works[index]->size);
                cursor += works[index]->size;
            }
            status = sim_usb_write(work->app->usb, batch, size);
        }
        report_usb_tx_stats(work->app, count, size);
    } else {
        status = sim_ipc_write(
            &work->app->ipc, SIM_RECORD_WIRE_FRAME, work->frame, work->size
        );
    }
    for (size_t index = 0u; index < count; ++index)
        complete_tx_work(works[index], status);
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

static void ota_begin_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const wlh_host_ota_begin_response_t *response
) {
    app_t *app = context;
    (void)domain;
    (void)status;
    pthread_mutex_lock(&app->state_mutex);
    app->last_completion_result = result;
    app->ota_begin_done = true;
    if (response != NULL)
        app->ota_begin = *response;
    pthread_cond_broadcast(&app->state_changed);
    pthread_mutex_unlock(&app->state_mutex);
}

static void ota_query_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const wlh_host_ota_query_response_t *response
) {
    app_t *app = context;
    (void)domain;
    (void)status;
    pthread_mutex_lock(&app->state_mutex);
    app->last_completion_result = result;
    app->ota_query_done = true;
    if (response != NULL)
        app->ota_query = *response;
    pthread_cond_broadcast(&app->state_changed);
    pthread_mutex_unlock(&app->state_mutex);
}

static void ota_activate_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const uint8_t *payload,
    size_t payload_size
) {
    app_t *app = context;
    (void)domain;
    (void)status;
    (void)payload;
    (void)payload_size;
    pthread_mutex_lock(&app->state_mutex);
    app->ota_activate_done = true;
    app->ota_activate_result = result;
    pthread_cond_broadcast(&app->state_changed);
    pthread_mutex_unlock(&app->state_mutex);
}

static void ota_tx_ready(void *context) {
    app_t *app = context;
    pthread_mutex_lock(&app->state_mutex);
    app->ota_credit_events++;
    pthread_cond_broadcast(&app->state_changed);
    pthread_mutex_unlock(&app->state_mutex);
}

/* READY-time wired-ETH probe completion. Runs on the adapter executor thread
 * (same context as host_event; see the probe call site there). A peer without
 * the ETH service (e.g. ESP32-S3 Wi-Fi firmware) fails the RPC with
 * WLH_HOST_PROTOCOL_ERROR/WLH_STATUS_NOT_SUPPORTED and this returns without
 * touching any state, leaving the Wi-Fi flow untouched. */
static void eth_get_info_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const wlh_host_eth_info_t *info
) {
    app_t *app = context;
    static const uint8_t zero_mac[6] = {0u};
    bool first_detect = false;
    bool link_up = false;
    bool link_down = false;
    uint8_t mac[6] = {0u};

    if (result != WLH_HOST_OK || info == NULL ||
        memcmp(info->mac_address, zero_mac, sizeof(zero_mac)) == 0) {
        WLH_LOGI(
            "host-sim",
            "wired ETH probe result=%d domain=%u status=%d; staying in "
            "Wi-Fi mode",
            result,
            domain,
            status
        );
        return;
    }

    pthread_mutex_lock(&app->state_mutex);
    if (!atomic_load(&app->eth_mode)) {
        atomic_store(&app->eth_mode, true);
        first_detect = true;
    }
    if (info->link_state == WLH_HOST_ETH_LINK_STATE_UP) {
        link_up = !app->eth_link_up ||
                  memcmp(app->eth_mac, info->mac_address, sizeof(mac)) != 0;
        app->eth_link_up = true;
        app->connected = true;
        app->disconnected = false;
    } else {
        link_down = app->eth_link_up;
        app->eth_link_up = false;
        if (link_down) {
            app->connected = false;
            app->disconnected = true;
        }
    }
    memcpy(app->eth_mac, info->mac_address, sizeof(app->eth_mac));
    memcpy(mac, info->mac_address, sizeof(mac));
    pthread_cond_broadcast(&app->state_changed);
    pthread_mutex_unlock(&app->state_mutex);

    if (first_detect)
        WLH_LOGI(
            "host-sim",
            "wired ETH service detected "
            "mac=%02x:%02x:%02x:%02x:%02x:%02x speed=%d duplex=%d link=%s; "
            "entering eth_mode",
            mac[0],
            mac[1],
            mac[2],
            mac[3],
            mac[4],
            mac[5],
            (int)info->speed,
            (int)info->duplex,
            info->link_state == WLH_HOST_ETH_LINK_STATE_UP ? "up" : "down"
        );
    /* A GET_INFO snapshot reporting link UP is applied directly instead of
     * waiting for a WLH_HOST_EVENT_ETH_LINK_STATE_CHANGED that may already
     * have fired before the probe completed. sim_network_link_up starts the
     * same DHCP flow as the WIFI_CONNECTED path. */
    if (link_up && app->network != NULL) {
        if (sim_network_link_up(app->network, mac) != 0)
            WLH_LOGW("host-sim", "failed to bring lwIP netif up");
    }
    if (link_down && app->network != NULL)
        sim_network_link_down(app->network);
    if (link_down && app->iperf != NULL)
        wlh_iperf_cancel(app->iperf, "wired ETH link down");
}

static void usb_on_frame(void *context, const uint8_t *frame, size_t size) {
    app_t *app = context;
    (void)wlh_host_on_frame(&app->host, frame, size);
}
static void usb_on_lost(void *context) {
    app_t *app = context;
    wlh_host_transport_lost(&app->host);
}

const char *wlh_app_wifi_security_name(wlh_protocol_v1_WifiSecurity security) {
    switch (security) {
    case wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_OPEN:
        return "Open";
    case wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WEP:
        return "WEP";
    case wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA_PSK:
        return "WPA-PSK";
    case wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA2_PSK:
        return "WPA2-PSK";
    case wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA_WPA2_PSK:
        return "WPA/WPA2-PSK";
    case wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA3_SAE:
        return "WPA3-SAE";
    case wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA2_WPA3_PSK:
        return "WPA2/WPA3-PSK";
    case wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_OWE:
        return "OWE";
    case wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA2_ENTERPRISE:
        return "WPA2-Enterprise";
    case wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA3_ENTERPRISE:
        return "WPA3-Enterprise";
    case wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_UNSPECIFIED:
    default:
        return "Unknown";
    }
}

int wlh_app_for_each_scan_network(
    const wlh_host_event_t *event,
    wlh_app_scan_network_fn callback,
    void *context
) {
    wlh_protocol_v1_WifiScanResultEvent result =
        wlh_protocol_v1_WifiScanResultEvent_init_zero;
    pb_istream_t input =
        pb_istream_from_buffer(event->payload, event->payload_size);
    pb_size_t index;

    if (!pb_decode(&input, wlh_protocol_v1_WifiScanResultEvent_fields, &result))
        return -1;
    for (index = 0; index < result.networks_count; ++index)
        callback(context, result.scan_id, &result.networks[index]);
    return 0;
}

static void log_scan_network(
    void *context, uint32_t scan_id, const wlh_protocol_v1_WifiNetwork *network
) {
    char ssid[sizeof(network->ssid.bytes) + 1u];
    pb_size_t ssid_index;
    (void)context;
    for (ssid_index = 0; ssid_index < network->ssid.size; ++ssid_index) {
        uint8_t byte = network->ssid.bytes[ssid_index];
        ssid[ssid_index] = (byte >= 0x20u && byte <= 0x7eu) ? (char)byte : '.';
    }
    ssid[network->ssid.size] = '\0';
    if (network->bssid.size != 6u) {
        WLH_LOGW("host-sim", "scan result has invalid BSSID length");
        return;
    }
    WLH_LOGI(
        "host-sim",
        "Wi-Fi scan result scan_id=%u ssid=%s "
        "bssid=%02x:%02x:%02x:%02x:%02x:%02x channel=%u rssi=%ld "
        "security=%s",
        scan_id,
        ssid,
        network->bssid.bytes[0],
        network->bssid.bytes[1],
        network->bssid.bytes[2],
        network->bssid.bytes[3],
        network->bssid.bytes[4],
        network->bssid.bytes[5],
        network->channel,
        (long)network->rssi_dbm,
        wlh_app_wifi_security_name(network->security)
    );
}

static void log_scan_results(const wlh_host_event_t *event) {
    if (wlh_app_for_each_scan_network(event, log_scan_network, NULL) != 0)
        WLH_LOGW("host-sim", "failed to decode Wi-Fi scan result event");
}

static void host_event(void *context, const wlh_host_event_t *event) {
    app_t *app = context;
    bool link_up = false;
    bool link_down = false;
    bool probe_eth = false;
    uint8_t interface_mac[6] = {0};
    pthread_mutex_lock(&app->state_mutex);
    if (event->kind == WLH_HOST_EVENT_WIFI_SCAN_COMPLETED)
        app->scan_complete = true;
    if (event->kind == WLH_HOST_EVENT_WIFI_CONNECTED) {
        wlh_protocol_v1_WifiConnectedEvent connected =
            wlh_protocol_v1_WifiConnectedEvent_init_zero;
        pb_istream_t stream =
            pb_istream_from_buffer(event->payload, event->payload_size);
        app->connected = true;
        if (pb_decode(
                &stream, wlh_protocol_v1_WifiConnectedEvent_fields, &connected
            ) &&
            connected.has_link && connected.link.mac.size == 6u) {
            memcpy(interface_mac, connected.link.mac.bytes, 6u);
            link_up = true;
        }
    }
    if (event->kind == WLH_HOST_EVENT_WIFI_DISCONNECTED) {
        app->disconnected = true;
        link_down = true;
    }
    if (event->kind == WLH_HOST_EVENT_ETHERNET_STA_RX ||
        event->kind == WLH_HOST_EVENT_ETHERNET_ETH_RX) {
        app->ethernet_rx = true;
        if (app->network != NULL)
            (void)sim_network_input(
                app->network, event->payload, event->payload_size
            );
    }
    if (event->kind == WLH_HOST_EVENT_ETH_LINK_STATE_CHANGED &&
        atomic_load(&app->eth_mode) &&
        event->payload_size >= sizeof(wlh_host_eth_link_state_event_t)) {
        wlh_host_eth_link_state_event_t change;
        memcpy(&change, event->payload, sizeof(change));
        if (change.link_state == WLH_HOST_ETH_LINK_STATE_UP) {
            link_up = !app->eth_link_up;
            app->eth_link_up = true;
            app->connected = true;
            app->disconnected = false;
            memcpy(interface_mac, app->eth_mac, sizeof(interface_mac));
        } else if (change.link_state == WLH_HOST_ETH_LINK_STATE_DOWN) {
            link_down = app->eth_link_up;
            app->eth_link_up = false;
            if (link_down) {
                app->connected = false;
                app->disconnected = true;
            }
        }
    }
    if (event->kind == WLH_HOST_EVENT_USER_MESSAGE_RESULT)
        app->user_result_received = true;
    if (event->kind == WLH_HOST_EVENT_STATE_CHANGED &&
        event->state == WLH_HOST_STATE_READY)
        probe_eth = true;
    if (event->kind == WLH_HOST_EVENT_STATE_CHANGED &&
        event->state != WLH_HOST_STATE_READY)
        app->ota_left_ready = true;
    pthread_cond_broadcast(&app->state_changed);
    pthread_mutex_unlock(&app->state_mutex);
    if (link_up && app->network != NULL) {
        if (sim_network_link_up(app->network, interface_mac) != 0)
            WLH_LOGW("host-sim", "failed to bring lwIP netif up");
    } else if (event->kind == WLH_HOST_EVENT_WIFI_CONNECTED && !link_up) {
        WLH_LOGW("host-sim", "connected event omitted the STA MAC");
    }
    if (link_down && app->network != NULL)
        sim_network_link_down(app->network);
    if (link_down && app->iperf != NULL)
        wlh_iperf_cancel(
            app->iperf,
            event->kind == WLH_HOST_EVENT_ETH_LINK_STATE_CHANGED
                ? "wired ETH link down"
                : "Wi-Fi link disconnected"
        );
    /* host_event runs on the adapter executor thread (app->executor):
     * host-core posts every event and RPC completion to config.executor
     * (wl-hosted-core host.c dispatch_event/dispatch_completion) and never
     * invokes on_event inline or with core locks held. Issuing
     * wlh_host_eth_get_info() here is therefore safe: it only allocates a
     * request and enqueues an RPC job on the core queue, and the completion
     * is posted back to this same executor. No extra worker thread needed. */
    if (probe_eth &&
        wlh_host_eth_get_info(&app->host, eth_get_info_completion, app) !=
            WLH_HOST_OK)
        WLH_LOGW("host-sim", "wired ETH probe request rejected");
    if (event->kind == WLH_HOST_EVENT_WIFI_SCAN_RESULT)
        log_scan_results(event);
    if (event->kind == WLH_HOST_EVENT_OTA_PROGRESS) {
        wlh_protocol_v1_OtaProgressEvent progress =
            wlh_protocol_v1_OtaProgressEvent_init_zero;
        pb_istream_t stream =
            pb_istream_from_buffer(event->payload, event->payload_size);
        if (pb_decode(
                &stream, wlh_protocol_v1_OtaProgressEvent_fields, &progress
            )) {
            unsigned long percent =
                app->ota_image_size == 0u
                    ? 0u
                    : (unsigned long)(progress.bytes_received * 100u /
                                      app->ota_image_size);
            WLH_LOGI(
                "host-sim",
                "OTA progress: transfer=%lu state=%u bytes=%llu/%llu (%lu%%)",
                (unsigned long)progress.transfer_id,
                (unsigned)progress.state,
                (unsigned long long)progress.bytes_received,
                (unsigned long long)app->ota_image_size,
                percent
            );
        }
    }
    /* Ethernet payload events are a data-plane hot path. Logging every frame
     * serializes high-rate UDP receive onto stderr; link errors still log at
     * their source in network_send. */
    if (event->kind != WLH_HOST_EVENT_ETHERNET_STA_RX &&
        event->kind != WLH_HOST_EVENT_ETHERNET_AP_RX &&
        event->kind != WLH_HOST_EVENT_ETHERNET_ETH_RX)
        WLH_LOGI(
            "host-sim",
            "event kind=%d state=%d service=%u method=%u bytes=%zu",
            event->kind,
            event->state,
            event->service_id,
            event->method_id,
            event->payload_size
        );
    wlh_repl_on_host_event(app, event);
}

static int network_send(void *context, const uint8_t *frame, size_t size) {
    app_t *app = context;
    static uint32_t frame_count;
    static uint32_t failure_count;
    /* eth_mode selects the wired-ETH data channel (ETHERNET_ETH); Wi-Fi
     * firmware keeps the STA channel. eth_mode only transitions false->true
     * after a successful probe, before the netif ever comes up in ETH mode. */
    wlh_host_result_t result =
        atomic_load(&app->eth_mode)
            ? wlh_host_ethernet_eth_send(&app->host, frame, size)
            : wlh_host_ethernet_sta_send(&app->host, frame, size);
    ++frame_count;
    if (result != WLH_HOST_OK)
        ++failure_count;
    if (frame_count <= 5u || frame_count % 100u == 0u ||
        (result != WLH_HOST_OK &&
         (failure_count <= 5u || failure_count % 100u == 0u))) {
        WLH_LOGI(
            "host-sim",
            "lwIP ethernet output #%lu len=%u result=%d",
            (unsigned long)frame_count,
            (unsigned)size,
            (int)result
        );
    }
    if (result == WLH_HOST_OK)
        return 0;
    return result == WLH_HOST_NO_CREDIT || result == WLH_HOST_PENDING_FULL ? 1
                                                                           : -1;
}

static void network_ping_result(
    void *context, const sim_ping_result_t *result
) {
    app_t *app = context;
    wlh_sim_v1_SimPingResult message = wlh_sim_v1_SimPingResult_init_zero;
    if (wlh_repl_on_ping_result(app, result))
        return;
    if (!app->ipc.sideband) {
        WLH_LOGI(
            "host-sim",
            "ping %s (%s) %u/%u %s",
            result->hostname,
            result->address,
            result->received,
            result->transmitted,
            result->success ? "ok" : "fail"
        );
        return;
    }
    message.request_id = result->request_id;
    message.transmitted = result->transmitted;
    message.received = result->received;
    message.success = result->success;
    (void)snprintf(
        message.hostname, sizeof(message.hostname), "%s", result->hostname
    );
    (void)snprintf(
        message.address, sizeof(message.address), "%s", result->address
    );
    (void)snprintf(
        message.detail, sizeof(message.detail), "%s", result->detail
    );
    if (send_protobuf(
            app,
            SIM_RECORD_PING_RESULT,
            wlh_sim_v1_SimPingResult_fields,
            &message,
            wlh_sim_v1_SimPingResult_size
        ) != 0)
        WLH_LOGW("host-sim", "failed to send ping result");
}

static void handle_ping_command(
    app_t *app, const uint8_t *payload, size_t payload_size
) {
    wlh_sim_v1_SimPingCommand message = wlh_sim_v1_SimPingCommand_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
    if (!pb_decode(&stream, wlh_sim_v1_SimPingCommand_fields, &message) ||
        message.request_id == 0u || message.hostname[0] == '\0' ||
        message.count == 0u || message.count > 10u ||
        message.timeout_ms == 0u || message.timeout_ms > 60000u ||
        app->network == NULL ||
        sim_network_ping(
            app->network,
            message.request_id,
            message.hostname,
            message.count,
            message.timeout_ms
        ) != 0) {
        sim_ping_result_t result;
        memset(&result, 0, sizeof(result));
        result.request_id = message.request_id;
        (void)snprintf(
            result.hostname, sizeof(result.hostname), "%s", message.hostname
        );
        (void)snprintf(
            result.detail, sizeof(result.detail), "%s", "invalid ping command"
        );
        network_ping_result(app, &result);
    }
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
    runtime.uptime_ms = wlh_app_monotonic_ms() - app->started_ms;
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
        } else if (kind == SIM_RECORD_PING_COMMAND && app->ipc.sideband) {
            handle_ping_command(app, payload, payload_size);
        }
        free(payload);
    }
    atomic_store(&app->running, false);
    pthread_mutex_lock(&app->state_mutex);
    pthread_cond_broadcast(&app->state_changed);
    pthread_mutex_unlock(&app->state_mutex);
    return NULL;
}

struct timespec wlh_app_relative_duration_ms(uint64_t duration_ms) {
    struct timespec duration;
    duration.tv_sec = (time_t)(duration_ms / 1000u);
    duration.tv_nsec = (long)(duration_ms % 1000u) * 1000000L;
    return duration;
}

bool wlh_app_interrupted(void) {
    return atomic_load(&interrupted);
}

bool wlh_app_wait_until(
    app_t *app, bool (*predicate)(app_t *), uint32_t timeout_ms
) {
    uint64_t deadline = wlh_app_monotonic_ms() + timeout_ms;
    uint64_t next_monitor = 0u;
    bool done = false;

    pthread_mutex_lock(&app->state_mutex);
    while (atomic_load(&app->running) && !atomic_load(&interrupted)) {
        uint64_t now = wlh_app_monotonic_ms();
        uint64_t wake_at;
        struct timespec native_duration;
        done = predicate(app);
        if (done || now >= deadline)
            break;
        if (now >= next_monitor) {
            pthread_mutex_unlock(&app->state_mutex);
            send_runtime(app);
            now = wlh_app_monotonic_ms();
            next_monitor = now + app->monitor_interval_ms;
            pthread_mutex_lock(&app->state_mutex);
        }
        wake_at = deadline < next_monitor ? deadline : next_monitor;
        now = wlh_app_monotonic_ms();
        native_duration =
            wlh_app_relative_duration_ms(wake_at > now ? wake_at - now : 0u);
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

static bool ota_begin_done(app_t *app) {
    return app->ota_begin_done;
}
static bool ota_query_done(app_t *app) {
    return app->ota_query_done;
}
static bool ota_credit_available(app_t *app) {
    return app->ota_credit_events != app->ota_credit_seen;
}
static bool ota_activate_finished_or_rebooting(app_t *app) {
    return app->ota_activate_done || app->ota_left_ready;
}

int wlh_app_run_ota(
    app_t *app,
    const char *image_path,
    const char *expected_version,
    uint32_t timeout_ms,
    const char **fail_stage
) {
    FILE *file;
    long file_size;
    uint8_t *image = NULL;
    size_t offset = 0u;
    wlh_host_ota_begin_params_t params;
    uint32_t timeout = timeout_ms == 0u ? 30000u : timeout_ms;
    const char *stage = "read-image";
    file = fopen(image_path, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
        (file_size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        if (file != NULL)
            fclose(file);
        goto fail;
    }
    image = malloc((size_t)file_size);
    if (image == NULL ||
        fread(image, 1u, (size_t)file_size, file) != (size_t)file_size) {
        fclose(file);
        goto fail;
    }
    fclose(file);
    memset(&params, 0, sizeof(params));
    params.image_size = (uint64_t)file_size;
    app->ota_image_size = (uint64_t)file_size;
    params.target_version = expected_version;
    CC_SHA256(image, (CC_LONG)file_size, params.sha256);
    app->ota_query_done = false;
    stage = "query";
    if (!wlh_app_wait_until(app, ready, 10000u) ||
        wlh_host_ota_query(&app->host, ota_query_completion, app) !=
            WLH_HOST_OK ||
        !wlh_app_wait_until(app, ota_query_done, 5000u))
        goto fail;
    if (app->ota_query.transfer_id != 0u) {
        app->completions = 0u;
        stage = "abort-stale";
        if (wlh_host_ota_abort(
                &app->host, app->ota_query.transfer_id, completion, app
            ) != WLH_HOST_OK ||
            !wlh_app_wait_until(app, one_completion, 5000u))
            goto fail;
    }
    app->ota_begin_done = false;
    stage = "begin";
    if (wlh_host_ota_begin(&app->host, &params, ota_begin_completion, app) !=
            WLH_HOST_OK ||
        !wlh_app_wait_until(app, ota_begin_done, timeout) ||
        app->last_completion_result != WLH_HOST_OK)
        goto fail;
    stage = "stream";
    while (offset < (size_t)file_size) {
        size_t chunk = app->ota_begin.stream_chunk_size;
        wlh_host_result_t result;
        if (chunk == 0u)
            goto fail;
        if (chunk > (size_t)file_size - offset)
            chunk = (size_t)file_size - offset;
        do {
            result = wlh_host_ota_stream_send(
                &app->host,
                app->ota_begin.transfer_id,
                offset,
                image + offset,
                chunk
            );
            if (result == WLH_HOST_NO_CREDIT) {
                pthread_mutex_lock(&app->state_mutex);
                app->ota_credit_seen = app->ota_credit_events;
                pthread_mutex_unlock(&app->state_mutex);
                if (!wlh_app_wait_until(app, ota_credit_available, 10000u))
                    goto fail;
            }
        } while (result == WLH_HOST_NO_CREDIT && !interrupted);
        if (result != WLH_HOST_OK)
            goto fail;
        offset += chunk;
    }
    app->completions = 0u;
    stage = "finalize";
    if (wlh_host_ota_finalize(
            &app->host, app->ota_begin.transfer_id, offset, completion, app
        ) != WLH_HOST_OK ||
        !wlh_app_wait_until(app, one_completion, timeout))
        goto fail;
    app->ota_activate_done = false;
    app->ota_left_ready = false;
    stage = "activate";
    if (wlh_host_ota_activate(
            &app->host,
            app->ota_begin.transfer_id,
            true,
            ota_activate_completion,
            app
        ) != WLH_HOST_OK ||
        !wlh_app_wait_until(app, ota_activate_finished_or_rebooting, 10000u))
        goto fail;
    if (app->ota_activate_done && app->ota_activate_result != WLH_HOST_OK)
        goto fail;
    stage = "reboot";
    if (!wlh_app_wait_until(app, not_ready, 10000u) ||
        !wlh_app_wait_until(app, ready, timeout))
        goto fail;
    app->ota_query_done = false;
    stage = "verify-clean";
    if (wlh_host_ota_query(&app->host, ota_query_completion, app) !=
            WLH_HOST_OK ||
        !wlh_app_wait_until(app, ota_query_done, 5000u) ||
        app->last_completion_result != WLH_HOST_OK ||
        app->ota_query.transfer_id != 0u)
        goto fail;
    stage = "verify-version";
    if (expected_version != NULL &&
        strcmp(wlh_host_get_peer_version(&app->host), expected_version) != 0)
        goto fail;
    free(image);
    return 0;
fail:
    free(image);
    if (fail_stage != NULL)
        *fail_stage = stage;
    return -1;
}

static void usage(const char *program) {
    fprintf(
        stderr, "usage: %s --ipc connect:PATH|fd:N | --usb VID:PID\n", program
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
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if ((endpoint == NULL) == !app.use_usb) {
        usage(argv[0]);
        return 2;
    }
    /* Arm the REPL before wlh_host_start so the first READY state change is
     * already emitted as a JSON event line. */
    app.repl.active = true;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    /* Managed sessions are long-lived: a Manager disconnect must surface as
     * EPIPE from write(), never as a fatal SIGPIPE. */
    signal(SIGPIPE, SIG_IGN);

    WLH_LOG_INIT();
    WLH_LOGI(
        "host-sim",
        "starting endpoint=%s usb=%s rpc_timeout_ms=%u",
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
    app.osal_ops = wlh_posix_osal_ops(&app.osal);
    config.osal = app.osal_ops;
    config.executor = (wlh_executor_ops_t){
        &app.executor, sim_executor_post};
    // clang-format on

    config.on_event = host_event;
    config.event_context = &app;

    config.bluetooth_hci_rx = wlh_ble_hci_rx;
    config.bluetooth_hci_tx_ready = wlh_ble_hci_tx_ready;
    config.bluetooth_context = NULL;
    config.ota_stream_tx_ready = ota_tx_ready;
    config.ota_context = &app;

    config.max_frame_size = 4096u;
    config.rpc_timeout_ms = rpc_timeout_ms;
    config.heartbeat_timeout_ms = 5000u;
    config.max_pending_rpc = 8u;
    config.core_queue_depth = 64u;
    config.ethernet_tx_depth = 64u;
    config.stop_timeout_ms = 3000u;

    if (wlh_host_init(&app.host, &config) != WLH_HOST_OK)
        return 1;
    app.network = sim_network_create(
        &config.osal, network_send, network_ping_result, &app
    );
    if (app.network == NULL) {
        WLH_LOGE("host-sim", "lwIP initialization failed");
        return 1;
    }
    app.iperf = wlh_iperf_controller_create(&app);
    if (app.iperf == NULL) {
        WLH_LOGE("host-sim", "failed to create iPerf controller");
        sim_network_destroy(app.network);
        return 1;
    }
    app.started_ms = wlh_app_monotonic_ms();
    atomic_store(&app.running, true);

    if (!app.use_usb &&
        pthread_create(&app.rx_thread, NULL, rx_main, &app) != 0)
        return 1;
    result = wlh_host_start(&app.host);

    if (result == WLH_HOST_OK)
        result = wlh_repl_run(&app);
    send_runtime(&app);

    wlh_iperf_controller_destroy(app.iperf);
    sim_network_destroy(app.network);
    app.network = NULL;
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
