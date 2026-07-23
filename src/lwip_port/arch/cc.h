#ifndef WLH_LWIP_ARCH_CC_H
#define WLH_LWIP_ARCH_CC_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#define LWIP_DONT_PROVIDE_BYTEORDER_FUNCTIONS
#define LWIP_RAND() ((unsigned int)rand())
#define LWIP_PLATFORM_DIAG(x)                                                  \
    do {                                                                       \
        printf x;                                                              \
    } while (0)
#define LWIP_PLATFORM_ASSERT(message)                                          \
    do {                                                                       \
        fprintf(                                                               \
            stderr,                                                            \
            "lwIP assertion failed: %s (%s:%d)\n",                             \
            message,                                                           \
            __FILE__,                                                          \
            __LINE__                                                           \
        );                                                                     \
        abort();                                                               \
    } while (0)

#endif
