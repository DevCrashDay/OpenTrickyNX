#ifndef SSX_LIVE_PRESS_H
#define SSX_LIVE_PRESS_H
#include <stdint.h>

/* A zero deadline is inactive; valid presses last less than 2^31 ms. */
static inline int live_press_pending(uint32_t until, uint32_t now)
{
    return until != 0 && (int32_t)(until - now) > 0;
}
#endif
