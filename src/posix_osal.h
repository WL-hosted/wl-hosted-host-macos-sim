#ifndef WLH_POSIX_OSAL_H
#define WLH_POSIX_OSAL_H

#include "wlh/osal.h"

typedef struct wlh_posix_osal {
    unsigned reserved;
} wlh_posix_osal_t;

void wlh_posix_osal_init(wlh_posix_osal_t *osal);
wlh_osal_ops_t wlh_posix_osal_ops(wlh_posix_osal_t *osal);

#endif
