/**
 * @file sleep.h
 * @brief Power policy: always-on / timed cycle / hibernate.
 *
 * Deep sleep can wake on MAIN (SoftAP), PAGE (BLE), or timer (cycle).
 */
#ifndef __SLEEP_H__
#define __SLEEP_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SLEEP_WAKE_NONE = 0,   /* power-on / HW RESET / SW restart */
    SLEEP_WAKE_TIMER,      /* cycle timer */
    SLEEP_WAKE_MAIN,       /* MAIN GPIO — open SoftAP after boot */
    SLEEP_WAKE_PAGE,       /* PAGE GPIO — open BLE after boot */
} sleep_wake_src_t;

/* Call once early in app_main (after config_open). */
void sleep_capture_wake_cause(void);

/* Peek / consume deep-sleep button wake intent (clears on consume). */
sleep_wake_src_t sleep_get_wake_src(void);
sleep_wake_src_t sleep_consume_wake_src(void);

/* If mode==CYCLE: schedule deep sleep after wake window. SoftAP/BLE cancel. */
void sleep_arm_from_config(void);
/* Once per boot after STA is online: optional wake URL + arm cycle (if CYCLE). */
void sleep_on_sta_online(void);
void sleep_cancel(void);
/* Refresh the CYCLE wake window (MQTT downlink / user activity). */
void sleep_kick(void);
/* Enter deep sleep now. SoftAP/BLE closed first. Wakes: MAIN, PAGE, or timer.
 * Returns false if SoftAP is up, already entering, or task spawn failed. */
bool sleep_now(void);
bool sleep_is_armed(void);

#ifdef __cplusplus
}
#endif

#endif /* __SLEEP_H__ */
