#ifndef WLH_NPL_H
#define WLH_NPL_H

#include <stdbool.h>

#include "wlh/osal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the OSAL backing every NPL primitive. Must run before any other
 * NPL call and stays valid until wlh_npl_deinit. Not reentrant. */
int wlh_npl_init(const wlh_osal_ops_t *ops);
void wlh_npl_deinit(void);

/* Marks the NimBLE host task as running; gates ble_npl_os_started. */
void wlh_npl_set_os_started(bool started);

#ifdef __cplusplus
}
#endif

#endif
