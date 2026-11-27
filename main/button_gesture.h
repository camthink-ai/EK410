/**
 * @file button_gesture.h
 * @brief Pure MAIN+PAGE hold ladder: release → power off, hold past 6 s → factory.
 *
 * Host-tested (tools/tests/test_button_gesture.c). Timing matches button.c.
 */
#ifndef __BUTTON_GESTURE_H__
#define __BUTTON_GESTURE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Keep in sync with button.c HOLD_*_MS. */
#define BTN_HOLD_SHUTDOWN_MS 2500
#define BTN_HOLD_FACTORY_MS  6000

typedef enum {
    BTN_BOTH_NONE = 0,
    BTN_BOTH_SHUTDOWN, /* release after shutdown hold, before factory */
    BTN_BOTH_FACTORY,  /* held past factory threshold (no release needed) */
} btn_both_action_t;

/** While both keys are still down. Returns FACTORY once both_ms crosses the line. */
btn_both_action_t btn_both_while_held(int both_ms);

/**
 * When either key is released after a BOTH lock.
 * Returns SHUTDOWN if the hold reached the shutdown floor and factory has not
 * already fired; otherwise NONE.
 */
btn_both_action_t btn_both_on_release(int both_ms, int factory_already);

#ifdef __cplusplus
}
#endif

#endif /* __BUTTON_GESTURE_H__ */
