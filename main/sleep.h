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
/* First time per boot that STA is online: optional wake URL + arm cycle (if
 * CYCLE). Later calls re-arm a cancelled CYCLE window and run a hibernate
 * that was saved while deferred. */
void sleep_on_sta_online(void);
/* CYCLE: bound this boot even when STA never gets an IP (router down, wrong
 * password). Arms the window now; a later GOT_IP runs the normal wake path. */
void sleep_arm_offline_fallback(void);
/* A SoftAP / BLE window just closed: re-arm CYCLE, bound an offline boot,
 * and run a hibernate that was saved while the window was up. */
void sleep_on_prov_closed(void);
/* Remember mode=hibernate saved while it could not run (SoftAP up / no STA);
 * the next sleep_on_sta_online() / sleep_on_prov_closed() enters it. */
void sleep_set_hibernate_pending(bool on);
/* MQTT cmd that owns the next sleep_now(): if that entry aborts, it gets a
 * follow-up error ack. Timer-driven entries never ack. */
void sleep_set_ack_cmd(const char *cmd);
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
