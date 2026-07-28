#ifndef WLH_NPL_NIMBLE_NPL_OS_H
#define WLH_NPL_NIMBLE_NPL_OS_H

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#include "os_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Must be a preprocessor-evaluable literal: os_mempool.h selects os_membuf_t
   with #if (OS_ALIGNMENT == N), so sizeof() cannot be used here. */
#define BLE_NPL_OS_ALIGNMENT (4)

#define BLE_NPL_TIME_FOREVER UINT32_MAX

struct ble_npl_eventq *ble_npl_eventq_dflt_get(void);

#ifdef __cplusplus
}
#endif

#endif
