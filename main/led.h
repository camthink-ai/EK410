/**
 * @file led.h
 * @brief Status LED — RGB on AW9523B (VE071A P1_1/G P1_2/R P1_3/B).
 *
 * Policy:
 *   blue blink  — SoftAP / Wi-Fi connecting (shared)
 *   green solid — running normally (STA up)
 *   red blink   — fault / factory reset
 *   purple blink— OTA in progress
 * MQTT does not drive the LED.
 */
#ifndef __LED_H__
#define __LED_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEDC_NET_R 0, 0, 255       /* blue: SoftAP / Wi-Fi joining */
#define LEDC_OK_R  0, 255, 0       /* green solid: running OK      */
#define LEDC_ERR_R 255, 0, 0       /* red: fault                   */
#define LEDC_OTA_R 180, 0, 180     /* purple: upgrading            */

/* Back-compat aliases used by older call sites. */
#define LEDC_BOOT_R LEDC_NET_R
#define LEDC_JOIN_R LEDC_NET_R
#define LEDC_AP_R   LEDC_NET_R

typedef enum {
    LED_OFF = 0,
    LED_ON,
    LED_BLINK,      /* 1 Hz  */
    LED_BLINK_FAST, /* 4 Hz */
} led_mode_t;

void led_set(uint8_t r, uint8_t g, uint8_t b, led_mode_t mode);
void led_pulse(uint8_t r, uint8_t g, uint8_t b, uint32_t ms);
void led_open(void);

#ifdef __cplusplus
}
#endif

#endif /* __LED_H__ */
