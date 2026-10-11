#ifndef TMAG_DEBUG_H
#define TMAG_DEBUG_H

#include <stdint.h>
#include <stdio.h>

/* Enable with EXTRA_DEFS="-DTMAG_INIT_DEBUG=1". In debug mode printf is
 * mirrored to USB CDC even when sensor output uses binary ROUND frames. */
#ifndef TMAG_INIT_DEBUG
#define TMAG_INIT_DEBUG 0
#endif
#ifndef TMAG_INIT_DEBUG_WAIT_MS
#define TMAG_INIT_DEBUG_WAIT_MS 5000U
#endif

/* May also be changed through a debugger before sensor initialization. */
extern volatile uint8_t g_tmag_init_debug;

#define TMAG_INIT_LOG(...) do { \
    if (g_tmag_init_debug != 0U) { \
        printf(__VA_ARGS__); \
        fflush(stdout); \
    } \
} while (0)

#endif
