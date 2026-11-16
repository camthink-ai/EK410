#ifndef __BOARD_IOEXT_H__
#define __BOARD_IOEXT_H__

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* AW9523B bring-up: panel power + RGB status LED (VE071A pin map). */
esp_err_t board_ioext_init(void);
bool board_ioext_ready(void);
esp_err_t board_ioext_epad_power(bool on);
/* Gate the VBAT divider into GPIO4 ADC (P0_6, active high). */
esp_err_t board_ioext_vbat_dect_en(bool on);

/* Drive RGB channels (0–255 logical). Scaled to board DIM ceiling. */
esp_err_t board_ioext_led_rgb(uint8_t r, uint8_t g, uint8_t b);

#endif
