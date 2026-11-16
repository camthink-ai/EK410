/**
 * @file battery.c
 * @brief VE071A battery sense: GPIO4 ADC1_CH2 gated by AW9523B P0_6.
 *
 * Matches board_config.h / ve071a_power.c: enable divider → settle → sample
 * with curve-fit calibration → ×2 → SOC lookup table.
 */
#include <string.h>
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "battery.h"
#include "board_ioext.h"
#include "pindefine.h"

#define TAG "-->BAT"
#define VBAT_ADC_UNIT       ADC_UNIT_1
#define VBAT_ADC_CHANNEL    ADC_CHANNEL_2 /* GPIO4 on ESP32-C61 */
#define VBAT_SETTLE_MS      50
#define VBAT_SAMPLES        8

typedef struct {
    uint16_t mv;
    uint8_t pct;
} soc_point_t;

/* Discharge OCV table from VE071A BSP (3087125HV 5000mAh). */
static const soc_point_t s_socTable[] = {
    { 4200, 100 }, { 4132, 95 }, { 4070, 90 }, { 4009, 85 },
    { 3949, 80 },  { 3890, 75 }, { 3834, 70 }, { 3779, 65 },
    { 3729, 60 },  { 3687, 55 }, { 3657, 50 }, { 3634, 45 },
    { 3616, 40 },  { 3599, 35 }, { 3582, 30 }, { 3563, 25 },
    { 3539, 20 },  { 3507, 15 }, { 3462, 10 }, { 3393, 5 },
    { 3326, 3 },   { 3100, 1 },  { 2800, 0 },
};
#define SOC_TABLE_LEN (sizeof(s_socTable) / sizeof(s_socTable[0]))

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_ready;

static uint8_t soc_lookup(uint16_t vbat_mv)
{
    if (vbat_mv >= s_socTable[0].mv) {
        return s_socTable[0].pct;
    }
    for (uint8_t i = 0; i < SOC_TABLE_LEN - 1; i++) {
        uint16_t hi = s_socTable[i].mv;
        uint16_t lo = s_socTable[i + 1].mv;
        if (vbat_mv >= lo && vbat_mv < hi) {
            uint32_t span_v = hi - lo;
            uint32_t span_p = s_socTable[i].pct - s_socTable[i + 1].pct;
            uint32_t into = vbat_mv - lo;
            return (uint8_t)(s_socTable[i + 1].pct + (into * span_p) / span_v);
        }
    }
    return s_socTable[SOC_TABLE_LEN - 1].pct;
}

static int ensure_adc(void)
{
    if (s_ready) {
        return 0;
    }
    adc_oneshot_unit_init_cfg_t unitCfg = {
        .unit_id = VBAT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&unitCfg, &s_adc) != ESP_OK) {
        return -1;
    }
    adc_oneshot_chan_cfg_t chCfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_oneshot_config_channel(s_adc, VBAT_ADC_CHANNEL, &chCfg) != ESP_OK) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return -1;
    }
    adc_cali_curve_fitting_config_t caliCfg = {
        .unit_id = VBAT_ADC_UNIT,
        .chan = VBAT_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&caliCfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC cali unavailable — using raw scale");
        s_cali = NULL;
    }
    s_ready = true;
    return 0;
}

int battery_read_mv(void)
{
    if (!board_ioext_ready()) {
        return -1;
    }
    if (ensure_adc() != 0) {
        return -1;
    }

    if (board_ioext_vbat_dect_en(true) != ESP_OK) {
        return -1;
    }
    TickType_t settle = pdMS_TO_TICKS(VBAT_SETTLE_MS);
    vTaskDelay(settle ? settle : 1);

    int sum = 0;
    int valid = 0;
    for (int i = 0; i < VBAT_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, VBAT_ADC_CHANNEL, &raw) != ESP_OK) {
            continue;
        }
        int pin_mv = 0;
        if (s_cali) {
            if (adc_cali_raw_to_voltage(s_cali, raw, &pin_mv) != ESP_OK) {
                continue;
            }
        } else {
            /* Fallback: 12-bit @ 12dB ≈ 0..3300 mV */
            pin_mv = raw * 3300 / 4095;
        }
        sum += pin_mv;
        valid++;
    }
    board_ioext_vbat_dect_en(false);

    if (valid == 0) {
        return -1;
    }
    int vbat = (sum / valid) * BAT_DIV_MULT;
    if (vbat < 0 || vbat > 4500) {
        ESP_LOGW(TAG, "unreasonable %d mV", vbat);
        return -1;
    }
    ESP_LOGI(TAG, "VBAT %d mV (n=%d)", vbat, valid);
    return vbat;
}

int battery_soc_from_mv(int mv)
{
    if (mv < 0) {
        return -1;
    }
    return (int)soc_lookup((uint16_t)mv);
}

int battery_read_percent(void)
{
    return battery_soc_from_mv(battery_read_mv());
}
