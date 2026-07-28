#ifndef WLH_NPL_NIMBLE_NPL_OS_LOG_H
#define WLH_NPL_NIMBLE_NPL_OS_LOG_H

#include <stdarg.h>
#include <stdio.h>

#define _WLH_NPL_LOG_LEVEL_DEBUG 0
#define _WLH_NPL_LOG_LEVEL_INFO 1
#define _WLH_NPL_LOG_LEVEL_WARN 2
#define _WLH_NPL_LOG_LEVEL_ERROR 3
#define _WLH_NPL_LOG_LEVEL_CRITICAL 4

/* DEBUG chatter from the stack is suppressed; everything else goes to
   stderr so it interleaves with EasyLogger output without reordering. */
#define BLE_NPL_LOG_IMPL(lvl)                                                  \
    static inline void                                                         \
    _BLE_NPL_LOG_CAT(BLE_NPL_LOG_MODULE, _BLE_NPL_LOG_CAT(_, lvl))(            \
        const char *fmt, ...                                                   \
    ) {                                                                        \
        if (_BLE_NPL_LOG_CAT(_WLH_NPL_LOG_LEVEL_, lvl) >=                      \
            _WLH_NPL_LOG_LEVEL_INFO) {                                         \
            va_list args;                                                      \
            va_start(args, fmt);                                               \
            (void)vfprintf(stderr, fmt, args);                                 \
            va_end(args);                                                      \
        }                                                                      \
    }

#endif
