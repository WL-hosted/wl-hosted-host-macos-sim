#ifndef WLH_BLE_APP_H
#define WLH_BLE_APP_H

#include <stdbool.h>
#include <stdint.h>

#include "wlh/host.h"
#include "wlh/osal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum wlh_ble_io_cap {
    WLH_BLE_IO_CAP_NO_IO = 0,
    WLH_BLE_IO_CAP_DISPLAY,
    WLH_BLE_IO_CAP_KEYBOARD,
    WLH_BLE_IO_CAP_DISPLAY_YES_NO
} wlh_ble_io_cap_t;

typedef struct wlh_ble_options {
    const char *bond_store_path; /* NULL selects the default location. */
    bool clear_bonds;
    wlh_ble_io_cap_t io_cap;
    bool have_passkey;
    uint32_t passkey;         /* 000000-999999 */
    const char *peer_address; /* "[public:|random:]aa:bb:cc:dd:ee:ff" */
    uint32_t timeout_ms;      /* per-step wait budget; 0 selects the default */
} wlh_ble_options_t;

/* Full plan 7.4 start flow: GET_INFO -> INITIALIZE(HCI) -> ENABLE(LE) ->
 * NPL/pools -> NimBLE host task -> sync. Requires a READY host session. */
int wlh_ble_app_start(
    wlh_host_t *host, const wlh_osal_ops_t *osal, const wlh_ble_options_t *opts
);

/* Full stop flow: stop scan/adv -> disconnect -> stop host task -> detach ->
 * DISABLE -> DEINITIALIZE(false). Safe to call after a failed start. */
void wlh_ble_app_stop(void);

int wlh_ble_run_central(void);
int wlh_ble_run_peripheral(void);

#ifdef __cplusplus
}
#endif

#endif
