/*
 * comm.c — VE071A/EK410 panel bus (standard SPI, dual software CS).
 *
 * Matches board/VE071A epd_io_esp + ve071a_spi:
 *   SPI2 @ 20 MHz, MOSI/MISO/SCLK, no QIO, no command_bits —
 *   EPD commands are ordinary data bytes. Full-duplex transfers.
 *   SPI_CTL selects EPD side of SGM3699; CS0/CS1 are GPIOs.
 */
#define __COMM_C__

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "comm.h"
#include "pindefine.h"
#include "status.h"

static const char *TAG = "comm";

/* Must be >= app_epd CHUNK_SIZE. */
#define SPI_MAX_BUFFER_SIZE 8192
#define STAGING_BUF_SIZE    64

static spi_device_handle_t s_spi;
static unsigned char *s_staging;

void spiCtlSelectEpd(int epd_side)
{
    gpio_config_t ctl = {
        .pin_bit_mask = 1ULL << SPI_CTL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&ctl);
    gpio_hold_dis(SPI_CTL);
    gpio_set_level(SPI_CTL, epd_side ? GPIO_HIGH : GPIO_LOW);
    esp_rom_delay_us(50);
}

static esp_err_t spi_tx(const void *tx, size_t len)
{
    if (!s_spi || !tx || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

static esp_err_t spi_duplex(const void *tx, void *rx, size_t len)
{
    if (!s_spi || !tx || !rx || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    spi_transaction_t t = {
        .length = len * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    return spi_device_polling_transmit(s_spi, &t);
}

unsigned char initialSpi(void)
{
    if (s_spi) {
        return DONE;
    }

    spiCtlSelectEpd(1);

    spi_bus_config_t buscfg = {
        .mosi_io_num = SPI_MOSI,
        .miso_io_num = SPI_MISO,
        .sclk_io_num = SPI_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SPI_MAX_BUFFER_SIZE,
        .flags = SPICOMMON_BUSFLAG_MASTER,
    };
    /* VE071A epd_io_esp: no command_bits, full duplex, software CS. */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 7,
    };

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(ret));
        return ERROR;
    }
    ret = spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device: %s", esp_err_to_name(ret));
        return ERROR;
    }

    s_staging = heap_caps_malloc(STAGING_BUF_SIZE, MALLOC_CAP_DMA);
    if (!s_staging) {
        return ERROR;
    }
    ESP_LOGI(TAG, "SPI2 ready 20 MHz (MOSI=%d MISO=%d SCLK=%d)", SPI_MOSI, SPI_MISO, SPI_CLK);
    return DONE;
}

void initialGpioDefault(void)
{
    gpio_config_t io = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    io.pin_bit_mask = (1ULL << EPD_RST) | (1ULL << SPI_MOSI) | (1ULL << SPI_MISO) |
                      (1ULL << SPI_CLK) | (1ULL << SPI_CS0) | (1ULL << SPI_CS1);
    gpio_config(&io);

    io.pin_bit_mask = (1ULL << EPD_DC);
    gpio_config(&io);

    io.pin_bit_mask = (1ULL << EPD_BUSY);
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
}

void initialGpio(void)
{
    gpio_set_level(SPI_CS0, GPIO_HIGH);
    gpio_set_direction(SPI_CS0, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(SPI_CS0, GPIO_PULLUP_ONLY);

    gpio_set_level(SPI_CS1, GPIO_HIGH);
    gpio_set_direction(SPI_CS1, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(SPI_CS1, GPIO_PULLUP_ONLY);

    gpio_set_level(EPD_RST, GPIO_HIGH);
    gpio_set_direction(EPD_RST, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(EPD_RST, GPIO_PULLUP_ONLY);
}

void delayms(unsigned int delayTime)
{
    vTaskDelay(pdMS_TO_TICKS(delayTime));
}

unsigned char spiTransmitCommand(unsigned char commandBuf)
{
    if (!s_staging) {
        return ERROR;
    }
    s_staging[0] = commandBuf;
    return (spi_tx(s_staging, 1) == ESP_OK) ? DONE : ERROR;
}

unsigned char spiTransmitData(const unsigned char *dataBuffer, unsigned long dataLength)
{
    while (dataLength > 0) {
        unsigned long n = dataLength;
        if (n > SPI_MAX_BUFFER_SIZE) {
            n = SPI_MAX_BUFFER_SIZE;
        }
        if (spi_tx(dataBuffer, n) != ESP_OK) {
            return ERROR;
        }
        dataBuffer += n;
        dataLength -= n;
    }
    return DONE;
}

unsigned char spiTransmitLargeData(unsigned char commandBuf, const unsigned char *dataBuffer, unsigned long dataLength)
{
    if (spiTransmitCommand(commandBuf) != DONE) {
        return ERROR;
    }
    return spiTransmitData(dataBuffer, dataLength);
}

unsigned char spiTransmit(unsigned char commandBuf, const unsigned char *epdData, unsigned int dataLength)
{
    if (dataLength == 0) {
        return spiTransmitCommand(commandBuf);
    }
    if (dataLength + 1 > STAGING_BUF_SIZE) {
        ESP_LOGE(TAG, "spiTransmit: %u bytes exceeds staging buffer", dataLength);
        return ERROR;
    }
    s_staging[0] = commandBuf;
    memcpy(s_staging + 1, epdData, dataLength);
    return (spi_tx(s_staging, dataLength + 1) == ESP_OK) ? DONE : ERROR;
}

unsigned char spiReceive(unsigned char commandBuf, unsigned char *dataBuffer, unsigned int dataLength)
{
    /* Full-duplex like epd_13in3_e6_check_ic_status: TX [cmd][0..] RX, status in byte1+. */
    static unsigned char buf[8] __attribute__((aligned(4)));
    if (!s_staging || dataLength + 1 > sizeof(buf)) {
        return ERROR;
    }
    memset(buf, 0, sizeof(buf));
    buf[0] = commandBuf;
    if (spi_duplex(buf, buf, dataLength + 1) != ESP_OK) {
        return ERROR;
    }
    memcpy(dataBuffer, buf + 1, dataLength);
    return DONE;
}

unsigned char spiReceiveData(unsigned char *dataBuffer, unsigned int dataLength)
{
    static unsigned char tx[8] __attribute__((aligned(4)));
    static unsigned char rx[8] __attribute__((aligned(4)));
    if (dataLength > sizeof(rx)) {
        return ERROR;
    }
    memset(tx, 0xFF, dataLength);
    if (spi_duplex(tx, rx, dataLength) != ESP_OK) {
        return ERROR;
    }
    memcpy(dataBuffer, rx, dataLength);
    return DONE;
}

void setGpioLevel(unsigned char pinNumber, unsigned char voltageLevel)
{
    gpio_set_level(pinNumber, voltageLevel);
}

unsigned char getGpioLevel(unsigned char pinNumber)
{
    return (unsigned char)gpio_get_level(pinNumber);
}
