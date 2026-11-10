#ifndef SSX_LIVE_PRESS_H
#define SSX_LIVE_PRESS_H
#include <stdint.h>

/* A zero deadline is inactive; valid presses last less than 2^31 ms. */
static inline uint32_t live_press_deadline(uint32_t now, uint32_t duration)
{
    uint32_t until = now + duration;
    return until ? until : 1; /* Reserve zero; at most one millisecond extra. */
}

/* The caller must poll at least once within 2^31 ms after a press expires. */
static inline int live_press_pending(uint32_t until, uint32_t now)
{
    return until != 0 && (int32_t)(until - now) > 0;
}
#endif
