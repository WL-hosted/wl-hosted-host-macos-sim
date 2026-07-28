#ifndef WLH_BLE_BOND_STORE_H
#define WLH_BLE_BOND_STORE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the bond file (default path when path is NULL), applies
 * clear_bonds, and installs the ble_hs_cfg store callbacks. */
int wlh_ble_bond_store_init(const char *path, bool clear_bonds);
void wlh_ble_bond_store_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
