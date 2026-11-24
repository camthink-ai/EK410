/**
 * AW9523B 驱动实现（16 位 I2C GPIO 扩展 + LED 恒流调光）
 *
 * 寄存器语义与接线注意见 aw9523b.h 顶部注释。
 * DIM 亮度寄存器与引脚的映射是乱序的（手册 Table 7）：
 *   P1_0~P1_3 → 0x20~0x23   P0_0~P0_7 → 0x24~0x2B   P1_4~P1_7 → 0x2C~0x2F
 * 由 s_dim_reg 表按下标直接查出，勿手算。
 *
 * 单脚方向/输出/模式/中断操作均为读-改-写（芯片支持标准寄存器回读，
 * 与 SGM31324 不同，无需影子寄存器）。
 */

#include "aw9523b.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "AW9523B";

/* DIM 亮度寄存器地址，下标 = aw9523b_pin_t */
static const uint8_t s_dim_reg[16] = {
    0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B,    // P0_0 ~ P0_7
    0x20, 0x21, 0x22, 0x23, 0x2C, 0x2D, 0x2E, 0x2F     // P1_0 ~ P1_3, P1_4 ~ P1_7
};

static i2c_master_dev_handle_t s_dev  = NULL;
static uint8_t                 s_addr  = AW9523B_I2C_ADDR;
/* Finite bus timeout: a stuck bus must not park the caller forever — callers
 * hold s_lock, and the sleep path powers the rails through this expander. */
#define AW9523B_I2C_TIMEOUT_MS 100

/* ── 引脚 → 寄存器/位 映射 ─────────────────────────────────────────────── */
static uint8_t pin_bit(aw9523b_pin_t pin)
{
    return (uint8_t)pin & 0x07;      // P0_0~P0_7 取低 3 位，P1_x 同
}

/* 口寄存器（INPUT/OUTPUT/CONFIG/INT_MASK/LED_MODE）每口一个寄存器，
 * 口内引脚由位掩码区分（pin_bit）——不能再加引脚位偏移。 */
static uint8_t pin_reg(aw9523b_pin_t pin, uint8_t reg_p0, uint8_t reg_p1)
{
    return (pin < 8) ? reg_p0 : reg_p1;
}

static uint8_t port_reg(aw9523b_port_t port, uint8_t reg_p0, uint8_t reg_p1)
{
    return (port == AW9523B_PORT0) ? reg_p0 : reg_p1;
}

/* 寄存器内单 bit 读-改-写 */
static esp_err_t reg_update_bit(aw9523b_pin_t pin, uint8_t reg_p0, uint8_t reg_p1,
                                bool set)
{
    uint8_t bit = 1u << pin_bit(pin);
    uint8_t reg = pin_reg(pin, reg_p0, reg_p1);
    uint8_t cur = 0;
    esp_err_t ret = aw9523b_read_reg(reg, &cur);
    if (ret != ESP_OK) return ret;
    return aw9523b_write_reg(reg, set ? (cur | bit) : (cur & (uint8_t)~bit));
}

/* ── 寄存器级通用读写 ─────────────────────────────────────────────────── */
esp_err_t aw9523b_read_reg(uint8_t reg, uint8_t *val)
{
    if (val == NULL) return ESP_ERR_INVALID_ARG;
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, AW9523B_I2C_TIMEOUT_MS);
}

esp_err_t aw9523b_write_reg(uint8_t reg, uint8_t val)
{
    if (s_dev == NULL) return ESP_ERR_INVALID_STATE;
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), AW9523B_I2C_TIMEOUT_MS);
}

esp_err_t aw9523b_update_reg(uint8_t reg, uint8_t mask, uint8_t val)
{
    uint8_t cur = 0;
    esp_err_t ret = aw9523b_read_reg(reg, &cur);
    if (ret != ESP_OK) return ret;
    return aw9523b_write_reg(reg, (uint8_t)((cur & (uint8_t)~mask) | (val & mask)));
}

/* ── 生命周期 ────────────────────────────────────────────────────────── */
esp_err_t aw9523b_init(const aw9523b_config_t *cfg)
{
    if (s_dev) return ESP_OK;                        /* 幂等：重复 init 直接 OK */
    if (!cfg || !cfg->bus) return ESP_ERR_INVALID_ARG;

    s_addr = cfg->addr ? cfg->addr : AW9523B_I2C_ADDR;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = s_addr,
        .scl_speed_hz    = AW9523B_I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(cfg->bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: 0x%x", ret);
        return ret;
    }

    /* 身份校验：ID 寄存器（0x10）固定读 0x23 */
    uint8_t id = 0;
    ret = aw9523b_read_reg(AW9523B_REG_ID, &id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "chip not responding at addr=0x%02X", s_addr);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ret;
    }
    if (id != 0x23) {
        ESP_LOGE(TAG, "wrong chip ID 0x%02X (expect 0x23)", id);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_INVALID_RESPONSE;   /* 有应答但身份不符 */
    }

    ESP_LOGI(TAG, "init ok, addr=0x%02X", s_addr);
    return ESP_OK;
}

bool aw9523b_is_ready(void)
{
    return s_dev != NULL;
}

esp_err_t aw9523b_deinit(void)
{
    esp_err_t ret = ESP_OK;
    if (s_dev) {
        ret = i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    ESP_LOGD(TAG, "deinit");
    return ret;
}

esp_err_t aw9523b_soft_reset(void)
{
    esp_err_t ret = aw9523b_write_reg(AW9523B_REG_SOFT_RESET, 0x00);
    if (ret != ESP_OK) return ret;
    /* 复位生效前不可访问。CONFIG_FREERTOS_HZ=100 时 pdMS_TO_TICKS(1)==0，
     * 兜底为 1 tick（10ms），同样覆盖复位窗口。 */
    TickType_t ticks = pdMS_TO_TICKS(1);
    vTaskDelay(ticks ? ticks : 1);
    return ESP_OK;
}

/* ── GPIO：方向 / 输出 / 输入 ─────────────────────────────────────────── */
esp_err_t aw9523b_set_port_dir(aw9523b_port_t port, uint8_t input_mask)
{
    return aw9523b_write_reg(port_reg(port, AW9523B_REG_CONFIG_P0, AW9523B_REG_CONFIG_P1),
                             input_mask);
}

esp_err_t aw9523b_set_pin_dir(aw9523b_pin_t pin, bool is_input)
{
    if ((uint8_t)pin > 15) return ESP_ERR_INVALID_ARG;
    return reg_update_bit(pin, AW9523B_REG_CONFIG_P0, AW9523B_REG_CONFIG_P1, is_input);
}

esp_err_t aw9523b_write_port(aw9523b_port_t port, uint8_t out_mask)
{
    return aw9523b_write_reg(port_reg(port, AW9523B_REG_OUTPUT_P0, AW9523B_REG_OUTPUT_P1),
                             out_mask);
}

esp_err_t aw9523b_write_pin(aw9523b_pin_t pin, bool level)
{
    if ((uint8_t)pin > 15) return ESP_ERR_INVALID_ARG;
    return reg_update_bit(pin, AW9523B_REG_OUTPUT_P0, AW9523B_REG_OUTPUT_P1, level);
}

esp_err_t aw9523b_read_port(aw9523b_port_t port, uint8_t *in_mask)
{
    if (in_mask == NULL) return ESP_ERR_INVALID_ARG;
    return aw9523b_read_reg(port_reg(port, AW9523B_REG_INPUT_P0, AW9523B_REG_INPUT_P1),
                            in_mask);
}

esp_err_t aw9523b_read_pin(aw9523b_pin_t pin, bool *level)
{
    if (level == NULL || (uint8_t)pin > 15) return ESP_ERR_INVALID_ARG;
    uint8_t val = 0;
    esp_err_t ret = aw9523b_read_reg(pin_reg(pin, AW9523B_REG_INPUT_P0, AW9523B_REG_INPUT_P1),
                                     &val);
    if (ret != ESP_OK) return ret;
    *level = (val >> pin_bit(pin)) & 0x01;
    return ESP_OK;
}

/* ── LED 恒流调光 ────────────────────────────────────────────────────── */
esp_err_t aw9523b_set_port_mode(aw9523b_port_t port, uint8_t gpio_mask)
{
    return aw9523b_write_reg(port_reg(port, AW9523B_REG_LED_MODE_P0, AW9523B_REG_LED_MODE_P1),
                             gpio_mask);
}

esp_err_t aw9523b_set_pin_mode(aw9523b_pin_t pin, aw9523b_pin_mode_t mode)
{
    if ((uint8_t)pin > 15) return ESP_ERR_INVALID_ARG;
    return reg_update_bit(pin, AW9523B_REG_LED_MODE_P0, AW9523B_REG_LED_MODE_P1,
                          mode == AW9523B_MODE_GPIO);
}

esp_err_t aw9523b_set_led_brightness(aw9523b_pin_t pin, uint8_t dim)
{
    if ((uint8_t)pin > 15) return ESP_ERR_INVALID_ARG;
    return aw9523b_write_reg(s_dim_reg[pin], dim);
}

esp_err_t aw9523b_set_current_range(aw9523b_current_t range)
{
    if ((uint8_t)range > AW9523B_CURRENT_1_4) return ESP_ERR_INVALID_ARG;
    return aw9523b_update_reg(AW9523B_REG_CTL, AW9523B_CTL_ISET_MASK, (uint8_t)range);
}

esp_err_t aw9523b_set_p0_drive_mode(aw9523b_drive_mode_t mode)
{
    return aw9523b_update_reg(AW9523B_REG_CTL, AW9523B_CTL_P0DRV_BIT,
                              mode == AW9523B_DRIVE_PUSH_PULL ? AW9523B_CTL_P0DRV_BIT : 0);
}

/* ── 中断 ────────────────────────────────────────────────────────────── */
esp_err_t aw9523b_set_port_int_mask(aw9523b_port_t port, uint8_t mask)
{
    return aw9523b_write_reg(port_reg(port, AW9523B_REG_INT_MASK_P0, AW9523B_REG_INT_MASK_P1),
                             mask);
}

esp_err_t aw9523b_enable_pin_interrupt(aw9523b_pin_t pin)
{
    if ((uint8_t)pin > 15) return ESP_ERR_INVALID_ARG;
    return reg_update_bit(pin, AW9523B_REG_INT_MASK_P0, AW9523B_REG_INT_MASK_P1, false);
}

esp_err_t aw9523b_disable_pin_interrupt(aw9523b_pin_t pin)
{
    if ((uint8_t)pin > 15) return ESP_ERR_INVALID_ARG;
    return reg_update_bit(pin, AW9523B_REG_INT_MASK_P0, AW9523B_REG_INT_MASK_P1, true);
}

esp_err_t aw9523b_clear_interrupt(void)
{
    /* 手册：读两口输入寄存器即复位 INT 触发 */
    uint8_t dummy = 0;
    esp_err_t ret = aw9523b_read_reg(AW9523B_REG_INPUT_P0, &dummy);
    if (ret != ESP_OK) return ret;
    return aw9523b_read_reg(AW9523B_REG_INPUT_P1, &dummy);
}

/* ── 调试 ────────────────────────────────────────────────────────────── */
void aw9523b_dump_regs(void)
{
    uint8_t in0 = 0, in1 = 0, out0 = 0, out1 = 0;
    uint8_t cfg0 = 0, cfg1 = 0, int0 = 0, int1 = 0;
    uint8_t led0 = 0, led1 = 0, ctl = 0, id = 0;

    if (aw9523b_read_reg(AW9523B_REG_INPUT_P0, &in0)     != ESP_OK ||
        aw9523b_read_reg(AW9523B_REG_INPUT_P1, &in1)     != ESP_OK ||
        aw9523b_read_reg(AW9523B_REG_OUTPUT_P0, &out0)   != ESP_OK ||
        aw9523b_read_reg(AW9523B_REG_OUTPUT_P1, &out1)   != ESP_OK ||
        aw9523b_read_reg(AW9523B_REG_CONFIG_P0, &cfg0)   != ESP_OK ||
        aw9523b_read_reg(AW9523B_REG_CONFIG_P1, &cfg1)   != ESP_OK ||
        aw9523b_read_reg(AW9523B_REG_INT_MASK_P0, &int0) != ESP_OK ||
        aw9523b_read_reg(AW9523B_REG_INT_MASK_P1, &int1) != ESP_OK ||
        aw9523b_read_reg(AW9523B_REG_LED_MODE_P0, &led0) != ESP_OK ||
        aw9523b_read_reg(AW9523B_REG_LED_MODE_P1, &led1) != ESP_OK ||
        aw9523b_read_reg(AW9523B_REG_CTL, &ctl)          != ESP_OK ||
        aw9523b_read_reg(AW9523B_REG_ID, &id)            != ESP_OK) {
        ESP_LOGE(TAG, "dump: read regs failed");
        return;
    }

    ESP_LOGI(TAG, "P0: IN=0x%02X OUT=0x%02X CFG=0x%02X INT=0x%02X LED=0x%02X",
             in0, out0, cfg0, int0, led0);
    ESP_LOGI(TAG, "P1: IN=0x%02X OUT=0x%02X CFG=0x%02X INT=0x%02X LED=0x%02X",
             in1, out1, cfg1, int1, led1);
    ESP_LOGI(TAG, "CTL=0x%02X ID=0x%02X", ctl, id);
}
