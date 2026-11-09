#include <assert.h>
#include "../src/live_press.h"

int main(void)
{
    assert(!live_press_pending(0, 0x80000001u));
    assert(live_press_pending(150, 100));
    assert(!live_press_pending(150, 150));
    assert(!live_press_pending(150, 151));
    assert(live_press_pending(20, 0xfffffff0u));
    assert(!live_press_pending(0xfffffff0u, 20));
    return 0;
}
