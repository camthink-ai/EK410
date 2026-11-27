/**
 * @file button_gesture.c
 * @brief MAIN+PAGE hold ladder (see button_gesture.h).
 */
#include "button_gesture.h"

btn_both_action_t btn_both_while_held(int both_ms)
{
    if (both_ms >= BTN_HOLD_FACTORY_MS) {
        return BTN_BOTH_FACTORY;
    }
    return BTN_BOTH_NONE;
}

btn_both_action_t btn_both_on_release(int both_ms, int factory_already)
{
    if (factory_already) {
        return BTN_BOTH_NONE;
    }
    /* Factory owns holds that reached 6 s; shutdown is the release window below. */
    if (both_ms >= BTN_HOLD_FACTORY_MS) {
        return BTN_BOTH_NONE;
    }
    if (both_ms >= BTN_HOLD_SHUTDOWN_MS) {
        return BTN_BOTH_SHUTDOWN;
    }
    return BTN_BOTH_NONE;
}
