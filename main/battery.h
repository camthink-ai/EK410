/**
 * @file battery.h
 * @brief Battery voltage sampling (ADC1, divider assumed equal to full-scale)
 */
#ifndef __BATTERY_H__
#define __BATTERY_H__

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pack voltage in mV (after divider restore), or -1 on failure. */
int battery_read_mv(void);

/* Map a pack mV reading to SOC % (0–100). */
int battery_soc_from_mv(int mv);

/* Sample once and return SOC %, or -1 when sense fails. */
int battery_read_percent(void);

#ifdef __cplusplus
}
#endif

#endif /* __BATTERY_H__ */
