#ifndef WLH_BLE_SYSCFG_H
#define WLH_BLE_SYSCFG_H

/*
 * WL-hosted NimBLE host configuration. Fixed limits follow the BLE
 * implementation plan; everything not overridden here falls through to the
 * pre-generated defaults in wlh_syscfg_base.h (taken verbatim from NimBLE
 * 1.10.0 porting/examples/linux).
 */

#define MYNEWT_VAL_BLE_MAX_CONNECTIONS (4)
#define MYNEWT_VAL_BLE_STORE_MAX_BONDS (16)
#define MYNEWT_VAL_BLE_STORE_MAX_CCCDS (64)
#define MYNEWT_VAL_BLE_ATT_PREFERRED_MTU (247)

/* Secure Connections only; legacy pairing stays compiled out. */
#define MYNEWT_VAL_BLE_SM_SC (1)
#define MYNEWT_VAL_BLE_SM_LEGACY (0)

/* Sized for MTU 247 across 4 connections. */
#define MYNEWT_VAL_MSYS_1_BLOCK_COUNT (32)
#define MYNEWT_VAL_MSYS_1_BLOCK_SIZE (300)

/* Mirror the 16/16 HCI slot budget of the WL-hosted HCI channel. */
#define MYNEWT_VAL_BLE_TRANSPORT_EVT_COUNT (16)
#define MYNEWT_VAL_BLE_TRANSPORT_EVT_DISCARDABLE_COUNT (16)
#define MYNEWT_VAL_BLE_TRANSPORT_EVT_SIZE (257)
#define MYNEWT_VAL_BLE_TRANSPORT_ACL_COUNT (16)
#define MYNEWT_VAL_BLE_TRANSPORT_ACL_FROM_HS_COUNT (16)
#define MYNEWT_VAL_BLE_TRANSPORT_ACL_FROM_LL_COUNT (16)
#define MYNEWT_VAL_BLE_TRANSPORT_ACL_SIZE (255)

#include "wlh_syscfg_base.h"

#endif
