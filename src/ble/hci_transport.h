#ifndef WLH_BLE_HCI_TRANSPORT_H
#define WLH_BLE_HCI_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

#include "wlh/host.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register these in wlh_host_config before wlh_host_init. They are inert
 * until wlh_ble_transport_attach runs. context is unused (single instance). */
wlh_host_result_t wlh_ble_hci_rx(
    void *context, uint8_t h4_type, const uint8_t *payload, size_t payload_size
);
void wlh_ble_hci_tx_ready(void *context);

/* Attach after nimble_port_init (transport pools and the default event queue
 * must exist) and before the NimBLE host starts issuing HCI commands. */
void wlh_ble_transport_attach(wlh_host_t *host);

/* Detach on stop or USB loss: drops all pending TX buffers and ignores any
 * further Core callbacks. Safe to call repeatedly. Must run while the NimBLE
 * host task is no longer processing (or before it starts). */
void wlh_ble_transport_detach(void);

#ifdef __cplusplus
}
#endif

#endif
