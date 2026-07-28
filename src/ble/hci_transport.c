/* Bridges the WL-hosted HCI channel to the NimBLE 1.10 transport framework.
 *
 * TX (NimBLE -> Core): ble_transport_to_ll_cmd/acl_impl run on whichever
 * task drives the NimBLE host. WLH_HOST_NO_CREDIT keeps ownership of the
 * NimBLE buffer in a bounded pending queue; bluetooth_hci_tx_ready posts an
 * event so the retry happens on the dedicated transport task (see below),
 * never on the Core task and never on the blockable NimBLE host task.
 *
 * RX (Core -> NimBLE): the Core callback only copies into a bounded ring and
 * signals; a dedicated RX task (never the NimBLE host task) allocates transport
 * buffers and delivers. The host task blocks inside ble_hs_hci_cmd_tx awaiting
 * the command-complete ack, so delivering acks from that same task would
 * deadlock; a separate task releases ble_hs_hci_sem while the host task waits.
 * The Core returns the channel credit whether or not the callback accepts a
 * packet, so reliable delivery is this adapter's job: the ring caps how many
 * best-effort advertising reports it holds so reliable HCI always finds a
 * slot, and the reliable-channel credit window is smaller than that reserve.
 * Transport pool exhaustion keeps the packet in the ring and retries via
 * callout.
 */

#include "hci_transport.h"

/* clang-format off */
/* os/os_mbuf.h must precede nimble/transport.h: the transport headers
   reference struct os_mbuf and os_mempool_put_fn without forward declaring
   them, so alphabetical include sorting here breaks the build. */
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "os/os_mbuf.h"
#include "nimble/transport.h"
#include "wlh/log.h"
/* clang-format on */

#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#define HCI_H4_CMD 0x01u
#define HCI_H4_ACL 0x02u
#define HCI_H4_EVT 0x04u

#define RX_RING_SLOTS 32u
/* Advertising reports never occupy more than this many ring slots; the rest
   stay reserved for reliable HCI. Must be at least the reliable-channel
   credit window (WLH_COPROC_BLUETOOTH_INITIAL_CREDIT, 16) away from
   RX_RING_SLOTS so a reliable event always finds a slot. */
#define RX_ADV_SLOTS_LIMIT 16u
#define RX_RETRY_TICKS 10u
#define TX_PENDING_ACL_SLOTS 16u
#define HCI_TRANSPORT_ERROR 0x07 /* BLE_ERR_MEM_CAPACITY */

typedef struct rx_slot {
    uint16_t size;
    uint8_t h4_type;
    uint8_t data[WLH_HOST_MAX_HCI_PACKET];
} rx_slot_t;

static wlh_host_t *transport_host;
static atomic_bool transport_attached;

/* Single-producer (Core task) / single-consumer (dedicated RX task) ring. */
static rx_slot_t rx_ring[RX_RING_SLOTS];
static atomic_uint_fast32_t rx_head;
static atomic_uint_fast32_t rx_tail;

/* Guarded by the NPL critical section: reached from the NimBLE host task and
   from any application task that issues GAP/GATT calls. */
static void *pending_cmd;
static struct os_mbuf *pending_acl[TX_PENDING_ACL_SLOTS];
static uint32_t pending_acl_head;
static uint32_t pending_acl_count;

static struct ble_npl_event tx_ready_event;
static struct ble_npl_event rx_event;
static struct ble_npl_callout rx_retry;

/* Dedicated transport task: services rx_event, tx_ready_event and the rx_retry
   callout on its own event queue so ack delivery never waits on the NimBLE
   host task, which blocks inside ble_hs_hci_cmd_tx. */
static const wlh_osal_ops_t *transport_osal;
static struct ble_npl_eventq transport_evq;
static wlh_osal_task_t transport_task;
static atomic_bool transport_task_running;
static bool transport_task_created;

static void transport_tx_drain(struct ble_npl_event *ev);
static void transport_rx_drain(struct ble_npl_event *ev);

static void transport_task_fn(void *argument) {
    (void)argument;
    while (atomic_load(&transport_task_running)) {
        struct ble_npl_event *event = ble_npl_eventq_get(&transport_evq, 50u);
        if (event != NULL)
            ble_npl_event_run(event);
    }
}

void wlh_ble_transport_attach(wlh_host_t *host, const wlh_osal_ops_t *osal) {
    wlh_osal_task_attributes_t attributes;
    atomic_store(&rx_head, 0u);
    atomic_store(&rx_tail, 0u);
    pending_cmd = NULL;
    memset(pending_acl, 0, sizeof(pending_acl));
    pending_acl_head = 0u;
    pending_acl_count = 0u;
    transport_osal = osal;
    ble_npl_eventq_init(&transport_evq);
    ble_npl_event_init(&tx_ready_event, transport_tx_drain, NULL);
    ble_npl_event_init(&rx_event, transport_rx_drain, NULL);
    ble_npl_callout_init(&rx_retry, &transport_evq, transport_rx_drain, NULL);
    transport_host = host;

    atomic_store(&transport_task_running, true);
    memset(&attributes, 0, sizeof(attributes));
    attributes.name = "ble-hci-rx";
    if (osal->task_create(
            osal->context, &transport_task, &attributes, transport_task_fn, NULL
        ) != 0) {
        atomic_store(&transport_task_running, false);
        transport_host = NULL;
        WLH_LOGE("host-ble", "cannot start HCI transport task");
        return;
    }
    transport_task_created = true;
    atomic_store(&transport_attached, true);
}

void wlh_ble_transport_detach(void) {
    uint32_t index;
    if (!atomic_exchange(&transport_attached, false))
        return;
    ble_npl_callout_stop(&rx_retry);
    if (transport_task_created) {
        atomic_store(&transport_task_running, false);
        (void)transport_osal->task_join(
            transport_osal->context, &transport_task, 3000u
        );
        transport_task_created = false;
    }
    if (pending_cmd != NULL) {
        ble_transport_free(pending_cmd);
        pending_cmd = NULL;
    }
    for (index = 0u; index < pending_acl_count; ++index) {
        uint32_t slot = (pending_acl_head + index) % TX_PENDING_ACL_SLOTS;
        os_mbuf_free_chain(pending_acl[slot]);
        pending_acl[slot] = NULL;
    }
    pending_acl_count = 0u;
    atomic_store(&rx_tail, atomic_load(&rx_head));
    transport_host = NULL;
}

/* Returns 0 on success/queued, -1 when the packet must stay with caller. */
static int transport_send_cmd(void *buf) {
    const uint8_t *packet = buf;
    size_t size = 3u + (size_t)packet[2];
    wlh_host_result_t result =
        wlh_host_bluetooth_hci_send(transport_host, HCI_H4_CMD, packet, size);
    if (result == WLH_HOST_OK) {
        ble_transport_free(buf);
        return 0;
    }
    if (result == WLH_HOST_NO_CREDIT)
        return -1;
    WLH_LOGW("host-ble", "HCI command send failed (%d)", (int)result);
    ble_transport_free(buf);
    return 0;
}

static int transport_send_acl(struct os_mbuf *om, bool *keep) {
    uint8_t flat[WLH_HOST_MAX_HCI_PACKET];
    uint16_t size = OS_MBUF_PKTLEN(om);
    wlh_host_result_t result;

    *keep = false;
    if (size > sizeof(flat) || os_mbuf_copydata(om, 0, (int)size, flat) != 0) {
        os_mbuf_free_chain(om);
        return HCI_TRANSPORT_ERROR;
    }
    result =
        wlh_host_bluetooth_hci_send(transport_host, HCI_H4_ACL, flat, size);
    if (result == WLH_HOST_OK) {
        os_mbuf_free_chain(om);
        return 0;
    }
    if (result == WLH_HOST_NO_CREDIT) {
        *keep = true;
        return 0;
    }
    WLH_LOGW("host-ble", "HCI ACL send failed (%d)", (int)result);
    os_mbuf_free_chain(om);
    return HCI_TRANSPORT_ERROR;
}

int ble_transport_to_ll_cmd_impl(void *buf) {
    uint32_t ctx;
    bool busy;
    if (!atomic_load(&transport_attached)) {
        ble_transport_free(buf);
        return HCI_TRANSPORT_ERROR;
    }
    ctx = ble_npl_hw_enter_critical();
    busy = pending_cmd != NULL;
    if (!busy && transport_send_cmd(buf) != 0)
        pending_cmd = buf;
    ble_npl_hw_exit_critical(ctx);
    if (busy) {
        /* HCI command flow control allows one outstanding command. */
        ble_transport_free(buf);
        return HCI_TRANSPORT_ERROR;
    }
    return 0;
}

int ble_transport_to_ll_acl_impl(struct os_mbuf *om) {
    uint32_t ctx;
    int rc = 0;
    bool keep = false;
    if (!atomic_load(&transport_attached)) {
        os_mbuf_free_chain(om);
        return HCI_TRANSPORT_ERROR;
    }
    ctx = ble_npl_hw_enter_critical();
    if (pending_acl_count > 0u) {
        keep = true; /* Preserve ordering behind queued packets. */
    } else {
        rc = transport_send_acl(om, &keep);
    }
    if (keep) {
        if (pending_acl_count < TX_PENDING_ACL_SLOTS) {
            uint32_t slot =
                (pending_acl_head + pending_acl_count) % TX_PENDING_ACL_SLOTS;
            pending_acl[slot] = om;
            pending_acl_count++;
        } else {
            os_mbuf_free_chain(om);
            rc = HCI_TRANSPORT_ERROR;
        }
    }
    ble_npl_hw_exit_critical(ctx);
    return rc;
}

int ble_transport_to_ll_iso_impl(struct os_mbuf *om) {
    os_mbuf_free_chain(om);
    return HCI_TRANSPORT_ERROR;
}

/* No LL-side state to construct: the controller lives on the coprocessor. */
void ble_transport_ll_init(void) {
}

static void transport_tx_drain(struct ble_npl_event *ev) {
    uint32_t ctx;
    (void)ev;
    if (!atomic_load(&transport_attached))
        return;
    ctx = ble_npl_hw_enter_critical();
    if (pending_cmd != NULL && transport_send_cmd(pending_cmd) == 0)
        pending_cmd = NULL;
    while (pending_acl_count > 0u) {
        bool keep = false;
        struct os_mbuf *om = pending_acl[pending_acl_head];
        (void)transport_send_acl(om, &keep);
        if (keep)
            break;
        pending_acl[pending_acl_head] = NULL;
        pending_acl_head = (pending_acl_head + 1u) % TX_PENDING_ACL_SLOTS;
        pending_acl_count--;
    }
    ble_npl_hw_exit_critical(ctx);
}

void wlh_ble_hci_tx_ready(void *context) {
    (void)context;
    if (!atomic_load(&transport_attached))
        return;
    ble_npl_eventq_put(&transport_evq, &tx_ready_event);
}

wlh_host_result_t wlh_ble_hci_rx(
    void *context, uint8_t h4_type, const uint8_t *payload, size_t payload_size
) {
    uint32_t head;
    uint32_t used;
    rx_slot_t *slot;
    (void)context;
    if (!atomic_load(&transport_attached))
        return WLH_HOST_INVALID_STATE;
    if (payload_size == 0u || payload_size > WLH_HOST_MAX_HCI_PACKET)
        return WLH_HOST_PROTOCOL_ERROR;
    head = (uint32_t)atomic_load_explicit(&rx_head, memory_order_relaxed);
    used =
        head - (uint32_t)atomic_load_explicit(&rx_tail, memory_order_acquire);
    if (used >= RX_RING_SLOTS)
        return WLH_HOST_PENDING_FULL;
    if (used >= RX_ADV_SLOTS_LIMIT && h4_type == HCI_H4_EVT &&
        wlh_hci_event_is_adv_report(payload, payload_size))
        return WLH_HOST_OK; /* Shed reports; keep slots for reliable HCI. */
    slot = &rx_ring[head % RX_RING_SLOTS];
    slot->h4_type = h4_type;
    slot->size = (uint16_t)payload_size;
    memcpy(slot->data, payload, payload_size);
    atomic_store_explicit(&rx_head, head + 1u, memory_order_release);
    ble_npl_eventq_put(&transport_evq, &rx_event);
    return WLH_HOST_OK;
}

static bool transport_deliver_evt(const rx_slot_t *slot) {
    void *buf;
    int discardable = wlh_hci_event_is_adv_report(slot->data, slot->size);
    if (slot->size < 2u || slot->size > MYNEWT_VAL(BLE_TRANSPORT_EVT_SIZE))
        return true; /* Cannot represent: count as consumed drop. */
    buf = ble_transport_alloc_evt(discardable);
    if (buf == NULL && discardable)
        return true; /* Advertising reports may be dropped under pressure. */
    if (buf == NULL)
        return false;
    memcpy(buf, slot->data, slot->size);
    if (ble_transport_to_hs_evt(buf) != 0)
        ble_transport_free(buf);
    return true;
}

static bool transport_deliver_acl(const rx_slot_t *slot) {
    struct os_mbuf *om = ble_transport_alloc_acl_from_ll();
    if (om == NULL)
        return false;
    if (os_mbuf_append(om, slot->data, slot->size) != 0) {
        os_mbuf_free_chain(om);
        return false;
    }
    if (ble_transport_to_hs_acl(om) != 0)
        os_mbuf_free_chain(om);
    return true;
}

static void transport_rx_drain(struct ble_npl_event *ev) {
    (void)ev;
    if (!atomic_load(&transport_attached))
        return;
    for (;;) {
        uint32_t tail =
            (uint32_t)atomic_load_explicit(&rx_tail, memory_order_relaxed);
        rx_slot_t *slot;
        bool consumed;
        if (tail ==
            (uint32_t)atomic_load_explicit(&rx_head, memory_order_acquire))
            return;
        slot = &rx_ring[tail % RX_RING_SLOTS];
        consumed = slot->h4_type == HCI_H4_EVT ? transport_deliver_evt(slot)
                                               : transport_deliver_acl(slot);
        if (!consumed) {
            /* Transport pools exhausted: retry once buffers recycle. */
            (void)ble_npl_callout_reset(&rx_retry, RX_RETRY_TICKS);
            return;
        }
        atomic_store_explicit(&rx_tail, tail + 1u, memory_order_release);
    }
}
