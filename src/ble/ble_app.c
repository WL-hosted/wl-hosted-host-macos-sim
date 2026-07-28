/* NimBLE application layer for the host simulator: lifecycle RPC gating,
 * host task, GATT test service, and the ble-central / ble-peripheral
 * scenario logic from BLE_IMPLEMENTATION_PLAN 7.4/7.7/7.8.
 *
 * This file is adapter code: pthread use is allowed here (unlike the NPL
 * port, which must stay on the OSAL contract).
 */

#include "ble_app.h"

#include "bond_store.h"
#include "hci_transport.h"
#include "npl/wlh_npl.h"
#include "wlh/log.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_sm.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLE_DEFAULT_STEP_TIMEOUT_MS 30000u
#define BLE_RPC_TIMEOUT_MS 5000u
#define BLE_VALUE_MAX 64u
#define BLE_PEER_NAME "wlh-ble-peer"
#define BLE_LOCAL_NAME "wlh-host-sim"
#define BLE_INIT_FEATURE_HCI 0x1u
#define BLE_ENABLE_MODE_LE 0x1u

/* 7f510000-1b15-4f0d-8a61-4c574c480001 */
static const ble_uuid128_t ble_test_svc_uuid = BLE_UUID128_INIT(
    0x01,
    0x00,
    0x48,
    0x4c,
    0x57,
    0x4c,
    0x61,
    0x8a,
    0x0d,
    0x4f,
    0x15,
    0x1b,
    0x00,
    0x00,
    0x51,
    0x7f
);
/* 7f510001-1b15-4f0d-8a61-4c574c480001 */
static const ble_uuid128_t ble_test_chr_uuid = BLE_UUID128_INIT(
    0x01,
    0x00,
    0x48,
    0x4c,
    0x57,
    0x4c,
    0x61,
    0x8a,
    0x0d,
    0x4f,
    0x15,
    0x1b,
    0x01,
    0x00,
    0x51,
    0x7f
);

typedef struct ble_app {
    wlh_host_t *host;
    const wlh_osal_ops_t *osal;
    wlh_ble_options_t options;

    pthread_mutex_t lock;
    pthread_cond_t cond;

    wlh_osal_task_t host_task;
    atomic_bool host_task_running;
    bool host_task_created;
    bool nimble_ready;    /* NPL + nimble_port_init + transport attached */
    bool rpc_initialized; /* INITIALIZE acknowledged, needs DEINITIALIZE */
    bool rpc_enabled;     /* ENABLE acknowledged, needs DISABLE */

    /* RPC gate */
    bool rpc_done;
    wlh_host_result_t rpc_result;
    int16_t rpc_status;
    bool info_done;
    wlh_host_result_t info_result;
    wlh_bluetooth_controller_info_t info;

    bool synced;

    /* Central link state */
    uint16_t conn_handle;
    bool connect_done;
    int connect_status;
    bool enc_done;
    int enc_status;
    bool enc_bonded;
    bool disconnected;

    bool peer_found;
    ble_addr_t peer_addr;
    bool scan_complete;

    /* Central discovery / GATT procedure state */
    bool svc_done;
    uint16_t svc_start_handle;
    uint16_t svc_end_handle;
    bool chr_done;
    uint16_t chr_val_handle;
    bool dsc_done;
    uint16_t cccd_handle;
    bool write_done;
    int write_status;
    bool read_done;
    int read_status;
    uint8_t read_value[BLE_VALUE_MAX];
    size_t read_size;
    bool notify_received;
    uint8_t notify_value[BLE_VALUE_MAX];
    size_t notify_size;

    /* Peripheral state */
    bool advertising;
    bool subscribed;
    unsigned transactions;
    uint16_t gatt_chr_val_handle;
    uint8_t gatt_value[BLE_VALUE_MAX];
    size_t gatt_value_size;
} ble_app_t;

static ble_app_t ble_app = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .conn_handle = BLE_HS_CONN_HANDLE_NONE,
};

static uint32_t step_timeout_ms(void) {
    return ble_app.options.timeout_ms != 0u ? ble_app.options.timeout_ms
                                            : BLE_DEFAULT_STEP_TIMEOUT_MS;
}

static bool ble_wait(bool (*predicate)(void), uint32_t timeout_ms) {
    struct timespec duration;
    bool done;

    pthread_mutex_lock(&ble_app.lock);
    done = predicate();
    while (!done) {
        duration.tv_sec = (time_t)(timeout_ms / 1000u);
        duration.tv_nsec = (long)(timeout_ms % 1000u) * 1000000L;
        if (pthread_cond_timedwait_relative_np(
                &ble_app.cond, &ble_app.lock, &duration
            ) != 0) {
            done = predicate();
            break;
        }
        done = predicate();
    }
    pthread_mutex_unlock(&ble_app.lock);
    return done;
}

static void ble_signal_locked(void) {
    pthread_cond_broadcast(&ble_app.cond);
}

/* ---- Lifecycle RPC gate -------------------------------------------------- */

static void rpc_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const uint8_t *payload,
    size_t payload_size
) {
    (void)context;
    (void)payload;
    (void)payload_size;
    (void)domain;
    pthread_mutex_lock(&ble_app.lock);
    ble_app.rpc_done = true;
    ble_app.rpc_result = result;
    ble_app.rpc_status = status;
    ble_signal_locked();
    pthread_mutex_unlock(&ble_app.lock);
}

static bool rpc_finished(void) {
    return ble_app.rpc_done;
}

static int await_rpc(const char *what, wlh_host_result_t submit) {
    if (submit != WLH_HOST_OK) {
        WLH_LOGE("host-ble", "%s rejected result=%d", what, (int)submit);
        return (int)submit;
    }
    if (!ble_wait(rpc_finished, BLE_RPC_TIMEOUT_MS)) {
        WLH_LOGE("host-ble", "%s timed out", what);
        return (int)WLH_HOST_TIMEOUT;
    }
    if (ble_app.rpc_result != WLH_HOST_OK) {
        WLH_LOGE(
            "host-ble",
            "%s failed result=%d status=%d",
            what,
            (int)ble_app.rpc_result,
            (int)ble_app.rpc_status
        );
        return (int)ble_app.rpc_result;
    }
    return 0;
}

static void info_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t status_domain,
    int16_t status_code,
    const wlh_bluetooth_controller_info_t *info
) {
    (void)context;
    (void)status_domain;
    (void)status_code;
    pthread_mutex_lock(&ble_app.lock);
    ble_app.info_done = true;
    ble_app.info_result = result;
    if (result == WLH_HOST_OK && info != NULL)
        ble_app.info = *info;
    ble_signal_locked();
    pthread_mutex_unlock(&ble_app.lock);
}

static bool info_finished(void) {
    return ble_app.info_done;
}

/* ---- NimBLE host callbacks ----------------------------------------------- */

static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);

    if (rc != 0)
        WLH_LOGW("host-ble", "ensure_addr failed rc=%d", rc);
    pthread_mutex_lock(&ble_app.lock);
    ble_app.synced = true;
    ble_signal_locked();
    pthread_mutex_unlock(&ble_app.lock);
    WLH_LOGI("host-ble", "NimBLE host synced with controller");
}

static void on_reset(int reason) {
    WLH_LOGW("host-ble", "NimBLE host reset reason=%d", reason);
    pthread_mutex_lock(&ble_app.lock);
    ble_app.synced = false;
    ble_signal_locked();
    pthread_mutex_unlock(&ble_app.lock);
}

static bool host_synced(void) {
    return ble_app.synced;
}

static void ble_host_task(void *argument) {
    (void)argument;
    wlh_npl_set_os_started(true);
    while (atomic_load(&ble_app.host_task_running)) {
        struct ble_npl_event *event =
            ble_npl_eventq_get(nimble_port_get_dflt_eventq(), 50u);
        if (event != NULL)
            ble_npl_event_run(event);
    }
}

/* ---- GATT test service ----------------------------------------------------
 */

static int gatt_access(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg
);

static struct ble_gatt_chr_def ble_test_chrs[2];
static struct ble_gatt_svc_def ble_test_svcs[2];

static void gatt_notify_pong(uint16_t conn_handle) {
    int rc =
        ble_gatts_notify_custom(conn_handle, ble_app.gatt_chr_val_handle, NULL);
    if (rc != 0)
        WLH_LOGW("host-ble", "notify failed rc=%d", rc);
}

static int gatt_access(
    uint16_t conn_handle,
    uint16_t attr_handle,
    struct ble_gatt_access_ctxt *ctxt,
    void *arg
) {
    (void)attr_handle;
    (void)arg;
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        int rc;
        pthread_mutex_lock(&ble_app.lock);
        rc = os_mbuf_append(
            ctxt->om, ble_app.gatt_value, (uint16_t)ble_app.gatt_value_size
        );
        pthread_mutex_unlock(&ble_app.lock);
        return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint8_t incoming[BLE_VALUE_MAX];
        uint16_t size = OS_MBUF_PKTLEN(ctxt->om);
        bool is_ping;
        if (size > sizeof(incoming))
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (os_mbuf_copydata(ctxt->om, 0, (int)size, incoming) != 0)
            return BLE_ATT_ERR_UNLIKELY;
        is_ping = size == 4u && memcmp(incoming, "ping", 4u) == 0;
        pthread_mutex_lock(&ble_app.lock);
        if (is_ping) {
            memcpy(ble_app.gatt_value, "pong", 4u);
            ble_app.gatt_value_size = 4u;
            ble_app.transactions++;
        } else {
            memcpy(ble_app.gatt_value, incoming, size);
            ble_app.gatt_value_size = size;
        }
        ble_signal_locked();
        pthread_mutex_unlock(&ble_app.lock);
        WLH_LOGI(
            "host-ble",
            "peripheral write received (%u bytes)%s",
            (unsigned)size,
            is_ping ? ", replying pong" : ""
        );
        if (is_ping)
            gatt_notify_pong(conn_handle);
        return 0;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static int gatt_register_test_service(bool mitm) {
    ble_gatt_chr_flags flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                               BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                               BLE_GATT_CHR_F_NOTIFY;
    int rc;

    if (mitm)
        flags |= BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_WRITE_AUTHEN;

    memset(ble_test_chrs, 0, sizeof(ble_test_chrs));
    ble_test_chrs[0].uuid = &ble_test_chr_uuid.u;
    ble_test_chrs[0].access_cb = gatt_access;
    ble_test_chrs[0].flags = flags;
    ble_test_chrs[0].val_handle = &ble_app.gatt_chr_val_handle;

    memset(ble_test_svcs, 0, sizeof(ble_test_svcs));
    ble_test_svcs[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
    ble_test_svcs[0].uuid = &ble_test_svc_uuid.u;
    ble_test_svcs[0].characteristics = ble_test_chrs;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    rc = ble_gatts_count_cfg(ble_test_svcs);
    if (rc == 0)
        rc = ble_gatts_add_svcs(ble_test_svcs);
    if (rc == 0)
        rc = ble_svc_gap_device_name_set(BLE_LOCAL_NAME);
    return rc;
}

/* ---- Start / stop ---------------------------------------------------------
 */

static bool link_disconnected(void);

int wlh_ble_app_start(
    wlh_host_t *host, const wlh_osal_ops_t *osal, const wlh_ble_options_t *opts
) {
    static const uint8_t sm_io_map[] = {
        BLE_SM_IO_CAP_NO_IO,
        BLE_SM_IO_CAP_DISP_ONLY,
        BLE_SM_IO_CAP_KEYBOARD_ONLY,
        BLE_SM_IO_CAP_DISP_YES_NO,
    };
    wlh_osal_task_attributes_t attributes;
    bool mitm;
    int rc;

    pthread_mutex_lock(&ble_app.lock);
    ble_app.host = host;
    ble_app.osal = osal;
    ble_app.options = *opts;
    ble_app.rpc_done = false;
    ble_app.info_done = false;
    ble_app.synced = false;
    ble_app.conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ble_app.transactions = 0u;
    memcpy(ble_app.gatt_value, "idle", 4u);
    ble_app.gatt_value_size = 4u;
    pthread_mutex_unlock(&ble_app.lock);

    rc = 0;
    if (wlh_host_bluetooth_get_info(host, info_completion, NULL) != WLH_HOST_OK)
        rc = (int)WLH_HOST_NOT_SUPPORTED;
    else if (!ble_wait(info_finished, BLE_RPC_TIMEOUT_MS))
        rc = (int)WLH_HOST_TIMEOUT;
    else if (ble_app.info_result != WLH_HOST_OK)
        rc = (int)ble_app.info_result;
    if (rc != 0) {
        WLH_LOGE("host-ble", "bluetooth GET_INFO failed result=%d", rc);
        return rc;
    }
    WLH_LOGI(
        "host-ble",
        "controller info: state=%d hci_version=%u manufacturer=0x%04x "
        "max_hci_packet=%u",
        (int)ble_app.info.state,
        ble_app.info.hci_version,
        ble_app.info.manufacturer_id,
        ble_app.info.max_hci_packet
    );

    ble_app.rpc_done = false;
    rc = await_rpc(
        "bluetooth INITIALIZE",
        wlh_host_bluetooth_initialize(
            host, BLE_INIT_FEATURE_HCI, rpc_completion, NULL
        )
    );
    if (rc != 0)
        return rc;
    ble_app.rpc_initialized = true;

    ble_app.rpc_done = false;
    rc = await_rpc(
        "bluetooth ENABLE",
        wlh_host_bluetooth_enable(
            host, BLE_ENABLE_MODE_LE, rpc_completion, NULL
        )
    );
    if (rc != 0)
        return rc;
    ble_app.rpc_enabled = true;

    if (wlh_npl_init(osal) != 0) {
        WLH_LOGE("host-ble", "NPL initialization failed");
        return -1;
    }
    nimble_port_init();
    wlh_ble_transport_attach(host);
    ble_app.nimble_ready = true;

    if (wlh_ble_bond_store_init(opts->bond_store_path, opts->clear_bonds) != 0)
        return -1;

    mitm = opts->io_cap != WLH_BLE_IO_CAP_NO_IO;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_io_cap = sm_io_map[opts->io_cap];
    ble_hs_cfg.sm_bonding = 1u;
    ble_hs_cfg.sm_mitm = mitm ? 1u : 0u;
    ble_hs_cfg.sm_sc = 1u;
    ble_hs_cfg.sm_our_key_dist =
        BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist =
        BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    rc = gatt_register_test_service(mitm);
    if (rc != 0) {
        WLH_LOGE("host-ble", "GATT registration failed rc=%d", rc);
        return -1;
    }

    atomic_store(&ble_app.host_task_running, true);
    memset(&attributes, 0, sizeof(attributes));
    attributes.name = "ble-host";
    if (osal->task_create(
            osal->context, &ble_app.host_task, &attributes, ble_host_task, NULL
        ) != 0) {
        atomic_store(&ble_app.host_task_running, false);
        WLH_LOGE("host-ble", "cannot start NimBLE host task");
        return -1;
    }
    ble_app.host_task_created = true;

    if (!ble_wait(host_synced, step_timeout_ms())) {
        WLH_LOGE("host-ble", "NimBLE sync timed out");
        return -1;
    }
    WLH_LOGI("host-ble", "bluetooth stack started");
    return 0;
}

void wlh_ble_app_stop(void) {
    wlh_host_t *host = ble_app.host;

    if (ble_app.nimble_ready && ble_app.synced) {
        uint16_t conn_handle;
        (void)ble_gap_disc_cancel();
        (void)ble_gap_adv_stop();
        pthread_mutex_lock(&ble_app.lock);
        conn_handle = ble_app.conn_handle;
        pthread_mutex_unlock(&ble_app.lock);
        if (conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            pthread_mutex_lock(&ble_app.lock);
            ble_app.disconnected = false;
            pthread_mutex_unlock(&ble_app.lock);
            if (ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM) == 0)
                (void)ble_wait(link_disconnected, 2000u);
        }
    }

    if (ble_app.host_task_created) {
        atomic_store(&ble_app.host_task_running, false);
        (void)ble_app.osal->task_join(
            ble_app.osal->context, &ble_app.host_task, 3000u
        );
        ble_app.host_task_created = false;
    }
    if (ble_app.nimble_ready) {
        wlh_ble_transport_detach();
        wlh_ble_bond_store_deinit();
        ble_app.nimble_ready = false;
    }

    if (host != NULL && ble_app.rpc_enabled) {
        ble_app.rpc_done = false;
        (void)await_rpc(
            "bluetooth DISABLE",
            wlh_host_bluetooth_disable(host, rpc_completion, NULL)
        );
        ble_app.rpc_enabled = false;
    }
    if (host != NULL && ble_app.rpc_initialized) {
        ble_app.rpc_done = false;
        (void)await_rpc(
            "bluetooth DEINITIALIZE",
            wlh_host_bluetooth_deinitialize(host, false, rpc_completion, NULL)
        );
        ble_app.rpc_initialized = false;
    }
    ble_app.synced = false;
    ble_app.host = NULL;
    WLH_LOGI("host-ble", "bluetooth stack stopped");
}

/* ---- Central --------------------------------------------------------------
 */

static bool parse_peer_address(const char *text, ble_addr_t *out) {
    unsigned values[6];
    memset(out, 0, sizeof(*out));
    out->type = BLE_ADDR_PUBLIC;
    if (strncmp(text, "public:", 7u) == 0) {
        text += 7;
    } else if (strncmp(text, "random:", 7u) == 0) {
        out->type = BLE_ADDR_RANDOM;
        text += 7;
    }
    if (sscanf(
            text,
            "%2x:%2x:%2x:%2x:%2x:%2x",
            &values[0],
            &values[1],
            &values[2],
            &values[3],
            &values[4],
            &values[5]
        ) != 6)
        return false;
    /* NimBLE addresses are little-endian. */
    out->val[5] = (uint8_t)values[0];
    out->val[4] = (uint8_t)values[1];
    out->val[3] = (uint8_t)values[2];
    out->val[2] = (uint8_t)values[3];
    out->val[1] = (uint8_t)values[4];
    out->val[0] = (uint8_t)values[5];
    return true;
}

static bool adv_matches_peer(const struct ble_gap_disc_desc *desc) {
    struct ble_hs_adv_fields fields;
    int index;

    if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) != 0)
        return false;
    for (index = 0; index < fields.num_uuids128; ++index)
        if (ble_uuid_cmp(&fields.uuids128[index].u, &ble_test_svc_uuid.u) == 0)
            return true;
    if (fields.name != NULL &&
        fields.name_len == (uint8_t)strlen(BLE_PEER_NAME) &&
        memcmp(fields.name, BLE_PEER_NAME, fields.name_len) == 0)
        return true;
    return false;
}

static int central_gap_event(struct ble_gap_event *event, void *arg);

static void handle_passkey_action(struct ble_gap_event *event) {
    struct ble_sm_io io;
    int rc;

    memset(&io, 0, sizeof(io));
    io.action = event->passkey.params.action;
    switch (event->passkey.params.action) {
    case BLE_SM_IOACT_DISP:
        io.passkey =
            ble_app.options.have_passkey ? ble_app.options.passkey : 123456u;
        WLH_LOGI("host-ble", "passkey to display: %06u", (unsigned)io.passkey);
        break;
    case BLE_SM_IOACT_INPUT:
        if (!ble_app.options.have_passkey) {
            WLH_LOGE("host-ble", "peer requires a passkey; use --ble-passkey");
            return;
        }
        io.passkey = ble_app.options.passkey;
        break;
    case BLE_SM_IOACT_NUMCMP:
        WLH_LOGI(
            "host-ble",
            "numeric comparison %06u auto-accepted",
            (unsigned)event->passkey.params.numcmp
        );
        io.numcmp_accept = 1u;
        break;
    default:
        return;
    }
    rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
    if (rc != 0)
        WLH_LOGW("host-ble", "ble_sm_inject_io failed rc=%d", rc);
}

static int central_gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        if (adv_matches_peer(&event->disc)) {
            pthread_mutex_lock(&ble_app.lock);
            if (!ble_app.peer_found) {
                ble_app.peer_found = true;
                ble_app.peer_addr = event->disc.addr;
                ble_signal_locked();
            }
            pthread_mutex_unlock(&ble_app.lock);
        }
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        pthread_mutex_lock(&ble_app.lock);
        ble_app.scan_complete = true;
        ble_signal_locked();
        pthread_mutex_unlock(&ble_app.lock);
        return 0;
    case BLE_GAP_EVENT_CONNECT:
        pthread_mutex_lock(&ble_app.lock);
        ble_app.connect_done = true;
        ble_app.connect_status = event->connect.status;
        if (event->connect.status == 0)
            ble_app.conn_handle = event->connect.conn_handle;
        ble_signal_locked();
        pthread_mutex_unlock(&ble_app.lock);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        WLH_LOGI(
            "host-ble", "disconnected reason=%d", event->disconnect.reason
        );
        pthread_mutex_lock(&ble_app.lock);
        ble_app.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_app.disconnected = true;
        ble_signal_locked();
        pthread_mutex_unlock(&ble_app.lock);
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        struct ble_gap_conn_desc desc;
        bool bonded = false;
        if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0)
            bonded = desc.sec_state.bonded;
        pthread_mutex_lock(&ble_app.lock);
        ble_app.enc_done = true;
        ble_app.enc_status = event->enc_change.status;
        ble_app.enc_bonded = bonded;
        ble_signal_locked();
        pthread_mutex_unlock(&ble_app.lock);
        return 0;
    }
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        handle_passkey_action(event);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t size = OS_MBUF_PKTLEN(event->notify_rx.om);
        pthread_mutex_lock(&ble_app.lock);
        if (size <= sizeof(ble_app.notify_value) &&
            os_mbuf_copydata(
                event->notify_rx.om, 0, (int)size, ble_app.notify_value
            ) == 0) {
            ble_app.notify_size = size;
            ble_app.notify_received = true;
            ble_signal_locked();
        }
        pthread_mutex_unlock(&ble_app.lock);
        return 0;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* Peer lost our bond: delete ours and accept a fresh pairing. */
        {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) ==
                0)
                ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    default:
        return 0;
    }
}

static bool peer_found(void) {
    return ble_app.peer_found;
}
static bool connect_finished(void) {
    return ble_app.connect_done;
}
static bool enc_finished(void) {
    return ble_app.enc_done;
}
static bool link_disconnected(void) {
    return ble_app.disconnected;
}
static bool svc_finished(void) {
    return ble_app.svc_done;
}
static bool chr_finished(void) {
    return ble_app.chr_done;
}
static bool dsc_finished(void) {
    return ble_app.dsc_done;
}
static bool write_finished(void) {
    return ble_app.write_done;
}
static bool read_finished(void) {
    return ble_app.read_done;
}
static bool notify_arrived(void) {
    return ble_app.notify_received;
}

static int on_svc_disc(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    const struct ble_gatt_svc *service,
    void *arg
) {
    (void)conn_handle;
    (void)arg;
    pthread_mutex_lock(&ble_app.lock);
    if (error->status == 0 && service != NULL) {
        ble_app.svc_start_handle = service->start_handle;
        ble_app.svc_end_handle = service->end_handle;
    } else {
        ble_app.svc_done = true;
        ble_signal_locked();
    }
    pthread_mutex_unlock(&ble_app.lock);
    return 0;
}

static int on_chr_disc(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    const struct ble_gatt_chr *chr,
    void *arg
) {
    (void)conn_handle;
    (void)arg;
    pthread_mutex_lock(&ble_app.lock);
    if (error->status == 0 && chr != NULL) {
        ble_app.chr_val_handle = chr->val_handle;
    } else {
        ble_app.chr_done = true;
        ble_signal_locked();
    }
    pthread_mutex_unlock(&ble_app.lock);
    return 0;
}

static int on_dsc_disc(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    uint16_t chr_val_handle,
    const struct ble_gatt_dsc *dsc,
    void *arg
) {
    (void)conn_handle;
    (void)chr_val_handle;
    (void)arg;
    pthread_mutex_lock(&ble_app.lock);
    if (error->status == 0 && dsc != NULL) {
        if (ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16)
            ble_app.cccd_handle = dsc->handle;
    } else {
        ble_app.dsc_done = true;
        ble_signal_locked();
    }
    pthread_mutex_unlock(&ble_app.lock);
    return 0;
}

static int on_write(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    struct ble_gatt_attr *attr,
    void *arg
) {
    (void)conn_handle;
    (void)attr;
    (void)arg;
    pthread_mutex_lock(&ble_app.lock);
    ble_app.write_done = true;
    ble_app.write_status = error->status;
    ble_signal_locked();
    pthread_mutex_unlock(&ble_app.lock);
    return 0;
}

static int on_read(
    uint16_t conn_handle,
    const struct ble_gatt_error *error,
    struct ble_gatt_attr *attr,
    void *arg
) {
    (void)conn_handle;
    (void)arg;
    pthread_mutex_lock(&ble_app.lock);
    ble_app.read_done = true;
    ble_app.read_status = error->status;
    if (error->status == 0 && attr != NULL) {
        uint16_t size = OS_MBUF_PKTLEN(attr->om);
        if (size <= sizeof(ble_app.read_value) &&
            os_mbuf_copydata(attr->om, 0, (int)size, ble_app.read_value) == 0)
            ble_app.read_size = size;
        else
            ble_app.read_status = BLE_ATT_ERR_UNLIKELY;
    }
    ble_signal_locked();
    pthread_mutex_unlock(&ble_app.lock);
    return 0;
}

int wlh_ble_run_central(void) {
    struct ble_gap_disc_params disc_params;
    uint8_t own_addr_type;
    uint16_t conn_handle;
    uint32_t timeout = step_timeout_ms();
    uint8_t cccd_value[2] = {0x01u, 0x00u};
    int rc;

    pthread_mutex_lock(&ble_app.lock);
    ble_app.peer_found = false;
    ble_app.scan_complete = false;
    ble_app.connect_done = false;
    ble_app.enc_done = false;
    ble_app.disconnected = false;
    ble_app.svc_done = false;
    ble_app.svc_start_handle = 0u;
    ble_app.svc_end_handle = 0u;
    ble_app.chr_done = false;
    ble_app.chr_val_handle = 0u;
    ble_app.dsc_done = false;
    ble_app.cccd_handle = 0u;
    ble_app.write_done = false;
    ble_app.read_done = false;
    ble_app.read_size = 0u;
    ble_app.notify_received = false;
    ble_app.notify_size = 0u;
    pthread_mutex_unlock(&ble_app.lock);

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        WLH_LOGE("host-ble", "no usable own address rc=%d", rc);
        return -1;
    }

    if (ble_app.options.peer_address != NULL) {
        if (!parse_peer_address(
                ble_app.options.peer_address, &ble_app.peer_addr
            )) {
            WLH_LOGE(
                "host-ble",
                "invalid --ble-peer-address %s",
                ble_app.options.peer_address
            );
            return -1;
        }
        ble_app.peer_found = true;
        WLH_LOGI(
            "host-ble",
            "using configured peer address %s",
            ble_app.options.peer_address
        );
    } else {
        memset(&disc_params, 0, sizeof(disc_params));
        disc_params.passive = 0u;
        disc_params.filter_duplicates = 1u;
        WLH_LOGI("host-ble", "central: scanning for test-service peer");
        rc = ble_gap_disc(
            own_addr_type,
            (int32_t)timeout,
            &disc_params,
            central_gap_event,
            NULL
        );
        if (rc != 0) {
            WLH_LOGE("host-ble", "scan start failed rc=%d", rc);
            return -1;
        }
        if (!ble_wait(peer_found, timeout)) {
            WLH_LOGE("host-ble", "central: no matching peer discovered");
            (void)ble_gap_disc_cancel();
            return -1;
        }
        (void)ble_gap_disc_cancel();
    }
    WLH_LOGI(
        "host-ble",
        "central: peer %02x:%02x:%02x:%02x:%02x:%02x (type %u)",
        ble_app.peer_addr.val[5],
        ble_app.peer_addr.val[4],
        ble_app.peer_addr.val[3],
        ble_app.peer_addr.val[2],
        ble_app.peer_addr.val[1],
        ble_app.peer_addr.val[0],
        ble_app.peer_addr.type
    );

    rc = ble_gap_connect(
        own_addr_type,
        &ble_app.peer_addr,
        (int32_t)timeout,
        NULL,
        central_gap_event,
        NULL
    );
    if (rc != 0) {
        WLH_LOGE("host-ble", "connect start failed rc=%d", rc);
        return -1;
    }
    if (!ble_wait(connect_finished, timeout) || ble_app.connect_status != 0) {
        WLH_LOGE(
            "host-ble",
            "central: connect failed status=%d",
            ble_app.connect_status
        );
        return -1;
    }
    conn_handle = ble_app.conn_handle;
    WLH_LOGI("host-ble", "central: connected handle=%u", conn_handle);

    rc = ble_gap_security_initiate(conn_handle);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        WLH_LOGE("host-ble", "security initiate failed rc=%d", rc);
        return -1;
    }
    if (!ble_wait(enc_finished, timeout) || ble_app.enc_status != 0) {
        WLH_LOGE(
            "host-ble",
            "central: encryption failed status=%d",
            ble_app.enc_status
        );
        return -1;
    }
    WLH_LOGI(
        "host-ble",
        "central: link encrypted (%s)",
        ble_app.enc_bonded ? "bond in use" : "new pairing"
    );

    rc = ble_gattc_disc_svc_by_uuid(
        conn_handle, &ble_test_svc_uuid.u, on_svc_disc, NULL
    );
    if (rc != 0 || !ble_wait(svc_finished, timeout) ||
        ble_app.svc_start_handle == 0u) {
        WLH_LOGE("host-ble", "central: test service not found");
        return -1;
    }
    rc = ble_gattc_disc_chrs_by_uuid(
        conn_handle,
        ble_app.svc_start_handle,
        ble_app.svc_end_handle,
        &ble_test_chr_uuid.u,
        on_chr_disc,
        NULL
    );
    if (rc != 0 || !ble_wait(chr_finished, timeout) ||
        ble_app.chr_val_handle == 0u) {
        WLH_LOGE("host-ble", "central: test characteristic not found");
        return -1;
    }
    rc = ble_gattc_disc_all_dscs(
        conn_handle,
        ble_app.chr_val_handle,
        ble_app.svc_end_handle,
        on_dsc_disc,
        NULL
    );
    if (rc != 0 || !ble_wait(dsc_finished, timeout) ||
        ble_app.cccd_handle == 0u) {
        WLH_LOGE("host-ble", "central: CCCD not found");
        return -1;
    }

    ble_app.write_done = false;
    rc = ble_gattc_write_flat(
        conn_handle,
        ble_app.cccd_handle,
        cccd_value,
        sizeof(cccd_value),
        on_write,
        NULL
    );
    if (rc != 0 || !ble_wait(write_finished, timeout) ||
        ble_app.write_status != 0) {
        WLH_LOGE("host-ble", "central: subscribe failed");
        return -1;
    }
    WLH_LOGI("host-ble", "central: subscribed to notifications");

    ble_app.write_done = false;
    rc = ble_gattc_write_flat(
        conn_handle, ble_app.chr_val_handle, "ping", 4u, on_write, NULL
    );
    if (rc != 0 || !ble_wait(write_finished, timeout) ||
        ble_app.write_status != 0) {
        WLH_LOGE("host-ble", "central: ping write failed");
        return -1;
    }
    if (!ble_wait(notify_arrived, timeout) || ble_app.notify_size != 4u ||
        memcmp(ble_app.notify_value, "pong", 4u) != 0) {
        WLH_LOGE("host-ble", "central: pong notification missing or wrong");
        return -1;
    }
    WLH_LOGI("host-ble", "central: pong notification received");

    rc = ble_gattc_read(conn_handle, ble_app.chr_val_handle, on_read, NULL);
    if (rc != 0 || !ble_wait(read_finished, timeout) ||
        ble_app.read_status != 0 || ble_app.read_size != 4u ||
        memcmp(ble_app.read_value, "pong", 4u) != 0) {
        WLH_LOGE("host-ble", "central: pong read verification failed");
        return -1;
    }
    WLH_LOGI("host-ble", "central: pong read back verified");

    rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0) {
        WLH_LOGE("host-ble", "terminate failed rc=%d", rc);
        return -1;
    }
    if (!ble_wait(link_disconnected, timeout)) {
        WLH_LOGE("host-ble", "central: disconnect not confirmed");
        return -1;
    }
    WLH_LOGI("host-ble", "central: scenario complete");
    return 0;
}

/* ---- Peripheral ------------------------------------------------------------
 */

static int peripheral_start_advertising(void);

static int peripheral_gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        WLH_LOGI(
            "host-ble", "peripheral: connect status=%d", event->connect.status
        );
        pthread_mutex_lock(&ble_app.lock);
        ble_app.advertising = false;
        if (event->connect.status == 0)
            ble_app.conn_handle = event->connect.conn_handle;
        ble_signal_locked();
        pthread_mutex_unlock(&ble_app.lock);
        if (event->connect.status != 0)
            (void)peripheral_start_advertising();
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        WLH_LOGI(
            "host-ble",
            "peripheral: disconnect reason=%d, re-advertising",
            event->disconnect.reason
        );
        pthread_mutex_lock(&ble_app.lock);
        ble_app.conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ble_app.subscribed = false;
        ble_signal_locked();
        pthread_mutex_unlock(&ble_app.lock);
        (void)peripheral_start_advertising();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        (void)peripheral_start_advertising();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        WLH_LOGI(
            "host-ble",
            "peripheral: encryption status=%d",
            event->enc_change.status
        );
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        pthread_mutex_lock(&ble_app.lock);
        ble_app.subscribed = event->subscribe.cur_notify != 0u;
        ble_signal_locked();
        pthread_mutex_unlock(&ble_app.lock);
        WLH_LOGI(
            "host-ble",
            "peripheral: notify subscription %s",
            event->subscribe.cur_notify != 0u ? "enabled" : "disabled"
        );
        return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        handle_passkey_action(event);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
            ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    default:
        return 0;
    }
}

static int peripheral_start_advertising(void) {
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp_fields;
    struct ble_gap_adv_params adv_params;
    uint8_t own_addr_type;
    int rc;

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0)
        return rc;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&ble_test_svc_uuid;
    fields.num_uuids128 = 1u;
    fields.uuids128_is_complete = 1u;
    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
        return rc;

    memset(&rsp_fields, 0, sizeof(rsp_fields));
    rsp_fields.name = (const uint8_t *)BLE_LOCAL_NAME;
    rsp_fields.name_len = (uint8_t)strlen(BLE_LOCAL_NAME);
    rsp_fields.name_is_complete = 1u;
    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0)
        return rc;

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(
        own_addr_type,
        NULL,
        BLE_HS_FOREVER,
        &adv_params,
        peripheral_gap_event,
        NULL
    );
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        pthread_mutex_lock(&ble_app.lock);
        ble_app.advertising = true;
        ble_signal_locked();
        pthread_mutex_unlock(&ble_app.lock);
        return 0;
    }
    WLH_LOGW("host-ble", "advertising start failed rc=%d", rc);
    return rc;
}

static bool transaction_seen(void) {
    return ble_app.transactions > 0u;
}

int wlh_ble_run_peripheral(void) {
    uint32_t timeout = step_timeout_ms();

    pthread_mutex_lock(&ble_app.lock);
    ble_app.transactions = 0u;
    ble_app.subscribed = false;
    memcpy(ble_app.gatt_value, "idle", 4u);
    ble_app.gatt_value_size = 4u;
    pthread_mutex_unlock(&ble_app.lock);

    if (peripheral_start_advertising() != 0)
        return -1;
    WLH_LOGI(
        "host-ble",
        "peripheral: advertising as %s, waiting up to %u ms",
        BLE_LOCAL_NAME,
        timeout
    );

    if (!ble_wait(transaction_seen, timeout)) {
        WLH_LOGE("host-ble", "peripheral: no ping/pong transaction observed");
        return -1;
    }
    WLH_LOGI(
        "host-ble",
        "peripheral: %u ping/pong transaction(s) served",
        ble_app.transactions
    );
    /* Stay reachable briefly so the peer can disconnect cleanly; advertising
     * resumes automatically after each disconnect. */
    (void)ble_wait(link_disconnected, 3000u);
    return 0;
}
