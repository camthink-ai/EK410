/**
 * @file led.c
 * @brief Status LED on AW9523B RGB (not WS2812).
 *
 * System colors (boot blue / AP yellow / OK green / …) are unchanged;
 * hardware sink is VE071A P1_1=G, P1_2=R, P1_3=B via board_ioext_led_rgb().
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

#include "led.h"
#include "board_ioext.h"

#define TAG "-->LED"
#define TICK_MS 50

typedef struct {
    uint8_t r, g, b;
    led_mode_t mode;
} led_state_t;

static bool s_ready;
static SemaphoreHandle_t s_mutex;
static led_state_t s_base;
static led_state_t s_pulse;
static uint32_t s_pulseTicks;

static void led_hw(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_ready) {
        return;
    }
    (void)board_ioext_led_rgb(r, g, b);
}

static void led_apply(const led_state_t *st, int tick)
{
    int on = 1;
    switch (st->mode) {
    case LED_OFF:
        led_hw(0, 0, 0);
        return;
    case LED_ON:
        on = 1;
        break;
    case LED_BLINK:
        on = (tick / 20) % 2; /* 1 Hz @ 50 ms */
        break;
    case LED_BLINK_FAST:
        on = (tick / 5) % 2; /* 4 Hz */
        break;
    }
    if (on) {
        led_hw(st->r, st->g, st->b);
    } else {
        led_hw(0, 0, 0);
    }
}

static void led_task(void *arg)
{
    int tick = 0;
    while (1) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        if (s_pulseTicks > 0) {
            s_pulseTicks--;
            led_apply(&s_pulse, tick);
        } else {
            led_apply(&s_base, tick);
        }
        xSemaphoreGive(s_mutex);
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
        tick++;
    }
}

void led_open(void)
{
    if (s_ready) {
        return;
    }
    if (board_ioext_init() != ESP_OK) {
        ESP_LOGE(TAG, "ioext init failed — status LED unavailable");
        return;
    }

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return;
    }
    s_base.mode = LED_OFF;
    s_ready = true;
    xTaskCreate(led_task, "led", 2048, NULL, 3, NULL);
    ESP_LOGI(TAG, "status LED ready (AW9523B RGB)");
}

static void set_state(led_state_t *slot, uint32_t *ticks,
                      uint8_t r, uint8_t g, uint8_t b, led_mode_t mode, uint32_t ms)
{
    if (!s_ready || !s_mutex) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    slot->r = r;
    slot->g = g;
    slot->b = b;
    slot->mode = mode;
    if (ticks) {
        *ticks = ms / TICK_MS;
    }
    xSemaphoreGive(s_mutex);
}

void led_set(uint8_t r, uint8_t g, uint8_t b, led_mode_t mode)
{
    set_state(&s_base, NULL, r, g, b, mode, 0);
}

void led_pulse(uint8_t r, uint8_t g, uint8_t b, uint32_t ms)
{
    set_state(&s_pulse, &s_pulseTicks, r, g, b, LED_ON, ms);
}
