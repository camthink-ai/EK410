/**
 * @file sleep.h
 * @brief Power policy: always-on / timed cycle / hibernate (sleep until MAIN).
 */
#ifndef __SLEEP_H__
#define __SLEEP_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* If mode==CYCLE: schedule deep sleep after wake window. SoftAP must cancel. */
void sleep_arm_from_config(void);
/* Once per boot after STA is online: optional wake URL + arm cycle (if CYCLE). */
void sleep_on_sta_online(void);
void sleep_cancel(void);
/* Enter deep sleep now: CYCLE uses timer+MAIN; else MAIN-only. SoftAP is closed first. */
void sleep_now(void);
/* True when a cycle (or hibernate-on-demand) timer is counting down. */
bool sleep_is_armed(void);

#ifdef __cplusplus
}
#endif

#endif /* __SLEEP_H__ */
