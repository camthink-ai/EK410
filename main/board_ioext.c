/**
 * AW9523B board I/O: panel power (P1_4) + RGB status LED (P1_1/2/3).
 * Pin map and LED current mode match VE071A board_config / ve071a_ioext.
 */
#include "board_ioext.h"
#include "pindefine.h"
#include "aw9523b.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "ioext";

#define IOEXT_EPAD_PWR_PIN      AW9523B_P1_4
#define IOEXT_VBAT_DECT_EN_PIN  AW9523B_P0_6
#define IOEXT_LED_G_PIN         AW9523B_P1_1
#define IOEXT_LED_R_PIN         AW9523B_P1_2
#define IOEXT_LED_B_PIN         AW9523B_P1_3

/* Soft ceiling: VE071A defaults (1/4 IMAX × DIM). */
#define IOEXT_LED_CURRENT_RANGE AW9523B_CURRENT_1_4
#define IOEXT_LED_DIM           24

#define P0_DIR_INIT             0xBF  /* only P0_6 output */
#define P1_DIR_INIT             0x41  /* P1_0/P1_6 input, rest output */
#define P0_OUT_INIT             0x00
#define P1_OUT_INIT             0x0E  /* power rails off */
/* P1 LEDMODE: bit1/2/3 = 0 → LED current mode; others GPIO. 0xF1 = ~0x0E */
#define P1_LEDMODE_INIT         0xF1

static i2c_master_bus_handle_t s_i2c;
static SemaphoreHandle_t s_lock;
static bool s_ready;

static esp_err_t locked_led_dim(aw9523b_pin_t pin, uint8_t dim)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    esp_err_t ret = aw9523b_set_led_brightness(pin, dim);
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return ret;
}

esp_err_t board_ioext_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << IOEXT_RST_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst);
    gpio_hold_dis(IOEXT_RST_GPIO);
    gpio_set_level(IOEXT_RST_GPIO, 0);
    esp_rom_delay_us(1000);
    gpio_set_level(IOEXT_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    if (!s_i2c) {
        i2c_master_bus_config_t bus = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = BOARD_I2C_SDA,
            .scl_io_num = BOARD_I2C_SCL,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        esp_err_t ret = i2c_new_master_bus(&bus, &s_i2c);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2c bus: %s", esp_err_to_name(ret));
            return ret;
        }
    }

    aw9523b_config_t cfg = { .bus = s_i2c, .addr = AW9523B_I2C_ADDR };
    esp_err_t ret = aw9523b_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "aw9523b init: %s", esp_err_to_name(ret));
        return ret;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }

    /* P0 all GPIO; P1 LED_G/R/B in constant-current LED mode. */
    aw9523b_set_port_mode(AW9523B_PORT0, 0xFF);
    aw9523b_set_port_mode(AW9523B_PORT1, P1_LEDMODE_INIT);
    aw9523b_set_p0_drive_mode(AW9523B_DRIVE_PUSH_PULL);
    aw9523b_set_current_range(IOEXT_LED_CURRENT_RANGE);

    aw9523b_write_port(AW9523B_PORT0, P0_OUT_INIT);
    aw9523b_write_port(AW9523B_PORT1, P1_OUT_INIT);
    aw9523b_set_port_dir(AW9523B_PORT0, P0_DIR_INIT);
    aw9523b_set_port_dir(AW9523B_PORT1, P1_DIR_INIT);

    aw9523b_set_led_brightness(IOEXT_LED_G_PIN, 0);
    aw9523b_set_led_brightness(IOEXT_LED_R_PIN, 0);
    aw9523b_set_led_brightness(IOEXT_LED_B_PIN, 0);

    aw9523b_set_port_int_mask(AW9523B_PORT0, 0xFF);
    aw9523b_set_port_int_mask(AW9523B_PORT1, 0xFF);
    aw9523b_clear_interrupt();

    s_ready = true;
    ESP_LOGI(TAG, "AW9523B ready @0x%02X (RGB LED mode)", AW9523B_I2C_ADDR);
    return ESP_OK;
}

bool board_ioext_ready(void)
{
    return s_ready;
}

esp_err_t board_ioext_epad_power(bool on)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    esp_err_t ret = aw9523b_write_pin(IOEXT_EPAD_PWR_PIN, on);
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "EPAD_PWR %s", on ? "ON" : "OFF");
    }
    return ret;
}

esp_err_t board_ioext_vbat_dect_en(bool on)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    esp_err_t ret = aw9523b_write_pin(IOEXT_VBAT_DECT_EN_PIN, on);
    if (s_lock) {
        xSemaphoreGive(s_lock);
    }
    return ret;
}

esp_err_t board_ioext_led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Scale 0–255 logical levels into the board DIM ceiling. */
    uint8_t dr = (uint8_t)(((uint32_t)r * IOEXT_LED_DIM + 127) / 255);
    uint8_t dg = (uint8_t)(((uint32_t)g * IOEXT_LED_DIM + 127) / 255);
    uint8_t db = (uint8_t)(((uint32_t)b * IOEXT_LED_DIM + 127) / 255);

    esp_err_t ret = locked_led_dim(IOEXT_LED_R_PIN, dr);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = locked_led_dim(IOEXT_LED_G_PIN, dg);
    if (ret != ESP_OK) {
        return ret;
    }
    return locked_led_dim(IOEXT_LED_B_PIN, db);
}
