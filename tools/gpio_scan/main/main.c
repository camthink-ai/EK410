/**
 * Button identity scan for EK410 / VE071A (ESP32-C61).
 * Only touches GPIOs known present on this board — no SoftAP / no reset logic.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"

#define TAG "BTNID"

/*
 * VE071A used GPIOs (do not treat as keys):
 * 7 DC, 8 SPI_CTL, 10/11 UART, 12 RST, 13 IOEXT_RST,
 * 22/28 I2C, 23/24 CS, 25/26/27 SPI, 29 BUSY
 *
 * Button candidates (board_config + leftover pads):
 *  2, 3 = documented BTN0/BTN1
 *  9    = CHIP BOOT
 *  0,1,4,5,6 = remaining possible front keys / strap
 */
static const int s_gpioCand[] = { 0, 1, 2, 3, 4, 5, 6, 9 };

#define AW9523B_ADDR 0x59
#define REG_INPUT_P0 0x00
#define REG_INPUT_P1 0x01
#define REG_ID       0x10

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_aw;
static bool s_awOk;

static esp_err_t aw_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_aw, &reg, 1, val, 1, 200);
}

static void aw_init(void)
{
    gpio_config_t rst = {
        .pin_bit_mask = 1ULL << 13,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&rst);
    gpio_set_level(13, 1);
    vTaskDelay(pdMS_TO_TICKS(30));

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = 22,
        .scl_io_num = 28,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        printf("I2C bus fail — GPIO-only\r\n");
        return;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AW9523B_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(s_bus, &dev, &s_aw) != ESP_OK) {
        printf("AW9523B add fail\r\n");
        return;
    }
    uint8_t id = 0;
    if (aw_read(REG_ID, &id) != ESP_OK) {
        printf("AW9523B no response\r\n");
        return;
    }
    s_awOk = true;
    printf("AW9523B ok id=0x%02X (watching P0/P1)\r\n", id);
}

static uint32_t sample_gpio(void)
{
    uint32_t m = 0;
    for (unsigned i = 0; i < sizeof(s_gpioCand) / sizeof(s_gpioCand[0]); i++) {
        int p = s_gpioCand[i];
        if (gpio_get_level(p) == 0) {
            m |= (1u << p);
        }
    }
    return m;
}

static void sample_aw(uint8_t *p0, uint8_t *p1)
{
    *p0 = *p1 = 0xFF;
    if (!s_awOk) {
        return;
    }
    (void)aw_read(REG_INPUT_P0, p0);
    (void)aw_read(REG_INPUT_P1, p1);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    printf("\r\n==== EK410 BUTTON ID SCAN ====\r\n");
    printf("Safe mode: no SoftAP / no factory reset / no reboot-on-hold.\r\n");
    fflush(stdout);

    for (unsigned i = 0; i < sizeof(s_gpioCand) / sizeof(s_gpioCand[0]); i++) {
        int p = s_gpioCand[i];
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << p,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t e = gpio_config(&io);
        printf("GPIO%d input+PU: %s\r\n", p, esp_err_to_name(e));
    }
    fflush(stdout);

    aw_init();
    fflush(stdout);

    uint32_t lastG = sample_gpio();
    uint8_t last0 = 0xFF, last1 = 0xFF;
    sample_aw(&last0, &last1);

    printf("Idle: GPIO_low=0x%03lX AW P0=0x%02X P1=0x%02X\r\n",
           (unsigned long)lastG, last0, last1);
    printf("\r\n>>> READY — short-press ONE key at a time, then release.\r\n");
    printf("Order: 1)MAIN  2)PAGE_UP  3)PAGE_DOWN  4)RESET\r\n");
    printf("Tell chat which key you pressed after each click.\r\n\r\n");
    fflush(stdout);

    int event = 0;
    while (1) {
        uint32_t nowG = sample_gpio();
        uint8_t now0 = 0xFF, now1 = 0xFF;
        sample_aw(&now0, &now1);

        if (nowG != lastG) {
            uint32_t diff = nowG ^ lastG;
            for (unsigned i = 0; i < sizeof(s_gpioCand) / sizeof(s_gpioCand[0]); i++) {
                int p = s_gpioCand[i];
                if ((diff >> p) & 1) {
                    int pressed = (nowG >> p) & 1;
                    event++;
                    printf("[%d] GPIO%d %s\r\n", event, p,
                           pressed ? "PRESSED" : "released");
                    fflush(stdout);
                }
            }
            lastG = nowG;
        }
        if (s_awOk && (now0 != last0 || now1 != last1)) {
            for (int b = 0; b < 8; b++) {
                if (((last0 ^ now0) >> b) & 1) {
                    event++;
                    printf("[%d] AW P0_%d %s\r\n", event, b,
                           ((now0 >> b) & 1) == 0 ? "PRESSED" : "released");
                    fflush(stdout);
                }
                if (((last1 ^ now1) >> b) & 1) {
                    event++;
                    printf("[%d] AW P1_%d %s\r\n", event, b,
                           ((now1 >> b) & 1) == 0 ? "PRESSED" : "released");
                    fflush(stdout);
                }
            }
            last0 = now0;
            last1 = now1;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
