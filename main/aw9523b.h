#ifndef __AW9523B_H__
#define __AW9523B_H__

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/**
 * Awinic AW9523B：16 位 I2C GPIO 扩展 + LED 恒流调光（P0/P1 各 8 脚）
 *
 * 每个引脚可独立选择工作模式（LEDMODE 寄存器 0x12/0x13 对应位）：
 *   1 = GPIO 模式（方向/输出/输入走 CONFIG/OUTPUT/INPUT 寄存器）
 *   0 = LED 模式（该脚变恒流沉，亮度由 DIM 寄存器 0x20~0x2F 线性控制，0~255）
 *
 * 中断：输入脚电平变化拉低 INT#（P0 变化 + P1 变化共用一根 INT 线）。
 *   INT_MASK 寄存器（0x06/0x07）对应位 1 = 屏蔽该脚中断，0 = 使能；
 *   读输入寄存器 0x00/0x01 即清中断。
 *
 * P1 口固定推挽输出；P0 口输出结构可选（CTL.D4），默认开漏，需上拉。
 *
 * 硬件复位脚（RST）由板级 GPIO 直接管理，不经过本驱动；驱动只提供
 * 软复位（写 0x7F）。初始化不预写任何配置寄存器，方向/模式/中断屏蔽
 * 由板级按需配置（同 tca9534a 思路）。
 */

/* ── 从机地址 ─────────────────────────────────────────────────────────────
 * 7-bit 地址 = 0x58 | (AD1<<1) | AD0，AD0/AD1 为芯片地址选择脚电平，
 * 本板拉线 AD1=0、AD0=1 → 0x59（与 uink_v2 参考工程一致）。
 * 板级若另行定义同名宏，此处跳过，板级优先。
 */
#ifndef AW9523B_I2C_ADDR
#define AW9523B_I2C_ADDR       0x59
#endif
#define AW9523B_I2C_FREQ_HZ    400000

/* ── 寄存器地址（手册 Register Map）─────────────────────────────────────── */
#define AW9523B_REG_INPUT_P0     0x00    // 输入端口 0（RO，读后清 INT）
#define AW9523B_REG_INPUT_P1     0x01    // 输入端口 1（RO，读后清 INT）
#define AW9523B_REG_OUTPUT_P0    0x02    // 输出锁存端口 0
#define AW9523B_REG_OUTPUT_P1    0x03    // 输出锁存端口 1
#define AW9523B_REG_CONFIG_P0    0x04    // 方向端口 0：1=输入 0=输出
#define AW9523B_REG_CONFIG_P1    0x05    // 方向端口 1：1=输入 0=输出
#define AW9523B_REG_INT_MASK_P0  0x06    // 中断屏蔽端口 0：1=屏蔽 0=使能
#define AW9523B_REG_INT_MASK_P1  0x07    // 中断屏蔽端口 1：1=屏蔽 0=使能
#define AW9523B_REG_ID           0x10    // 芯片 ID（RO，固定读 0x23）
#define AW9523B_REG_CTL          0x11    // D4=P0 推挽选择，[1:0]=全局电流档
#define AW9523B_REG_LED_MODE_P0  0x12    // 工作模式端口 0：1=GPIO 0=LED
#define AW9523B_REG_LED_MODE_P1  0x13    // 工作模式端口 1：1=GPIO 0=LED
#define AW9523B_REG_DIM_BASE     0x20    // DIM0~DIM15 亮度寄存器基址 0x20~0x2F
#define AW9523B_REG_SOFT_RESET   0x7F    // 写 0x00 软复位，寄存器恢复 POR

/* CTL（0x11）字段 */
#define AW9523B_CTL_P0DRV_BIT    0x10    // D4：1=P0 推挽 0=开漏
#define AW9523B_CTL_ISET_MASK    0x03    // D[1:0]：全局电流档（aw9523b_current_t 编码）

/* ── 引脚编号 ───────────────────────────────────────────────────────────── */
typedef enum {
    AW9523B_P0_0 = 0,  AW9523B_P0_1,  AW9523B_P0_2,  AW9523B_P0_3,
    AW9523B_P0_4,      AW9523B_P0_5,  AW9523B_P0_6,  AW9523B_P0_7,
    AW9523B_P1_0 = 8,  AW9523B_P1_1,  AW9523B_P1_2,  AW9523B_P1_3,
    AW9523B_P1_4,      AW9523B_P1_5,  AW9523B_P1_6,  AW9523B_P1_7,
} aw9523b_pin_t;

typedef enum {
    AW9523B_PORT0 = 0,     // P0_0 ~ P0_7
    AW9523B_PORT1,         // P1_0 ~ P1_7
} aw9523b_port_t;

/* ── 引脚工作模式（LEDMODE 寄存器对应位编码）───────────────────────────── */
typedef enum {
    AW9523B_MODE_GPIO = 1, // GPIO 模式
    AW9523B_MODE_LED  = 0, // LED 恒流调光模式
} aw9523b_pin_mode_t;

/* ── 全局电流档（CTL[1:0]，LED 模式恒流值按比例缩放）───────────────────── */
typedef enum {
    AW9523B_CURRENT_FULL = 0,  // 1×IMAX（满电流）
    AW9523B_CURRENT_3_4  = 1,  // 3/4 IMAX
    AW9523B_CURRENT_1_2  = 2,  // 1/2 IMAX
    AW9523B_CURRENT_1_4  = 3,  // 1/4 IMAX
} aw9523b_current_t;

/* ── P0 口输出结构（CTL.D4，P1 固定推挽）──────────────────────────────── */
typedef enum {
    AW9523B_DRIVE_OPEN_DRAIN = 0,  // 开漏（需外部上拉）
    AW9523B_DRIVE_PUSH_PULL  = 1,  // 推挽
} aw9523b_drive_mode_t;

/* ── 配置 ────────────────────────────────────────────────────────────────── */
typedef struct {
    i2c_master_bus_handle_t bus;    // 板级提供的 I2C 总线句柄
    uint8_t                 addr;   // 7-bit 地址，填 AW9523B_I2C_ADDR 或 0 用默认
} aw9523b_config_t;

/* ── 生命周期 ────────────────────────────────────────────────────────────── */
// 初始化：挂到 I2C 总线，读 ID 寄存器（0x10）校验 ==0x23。
// 不写任何配置寄存器，方向/模式/中断屏蔽由板级按需配置。
// 重复调用幂等（已初始化直接返回 ESP_OK）。
esp_err_t aw9523b_init(const aw9523b_config_t *cfg);
bool      aw9523b_is_ready(void);
esp_err_t aw9523b_deinit(void);     // 移除 I2C 设备（总线由板级统一管理）

// 软复位：写 0x7F=0x00，全部寄存器恢复 POR（内部延时 1ms 等复位生效）
esp_err_t aw9523b_soft_reset(void);

/* ── 寄存器级通用读写 ────────────────────────────────────────────────────── */
esp_err_t aw9523b_read_reg  (uint8_t reg, uint8_t *val);
esp_err_t aw9523b_write_reg (uint8_t reg, uint8_t val);
esp_err_t aw9523b_update_reg(uint8_t reg, uint8_t mask, uint8_t val);

/* ── GPIO：方向 / 输出 / 输入 ────────────────────────────────────────────── */
// 端口整写方向（CONFIG 寄存器）：mask 位 1=输入 0=输出
esp_err_t aw9523b_set_port_dir(aw9523b_port_t port, uint8_t input_mask);
esp_err_t aw9523b_set_pin_dir (aw9523b_pin_t pin, bool is_input);

// 端口整写输出锁存 / 单脚写（单脚为读-改-写）
esp_err_t aw9523b_write_port(aw9523b_port_t port, uint8_t out_mask);
esp_err_t aw9523b_write_pin (aw9523b_pin_t pin, bool level);

// 端口/单脚读输入电平
esp_err_t aw9523b_read_port(aw9523b_port_t port, uint8_t *in_mask);
esp_err_t aw9523b_read_pin (aw9523b_pin_t pin, bool *level);

/* ── LED 恒流调光 ────────────────────────────────────────────────────────── */
// 引脚工作模式：GPIO / LED 恒流调光（端口整写 mask 位 1=GPIO 0=LED）
esp_err_t aw9523b_set_port_mode(aw9523b_port_t port, uint8_t gpio_mask);
esp_err_t aw9523b_set_pin_mode (aw9523b_pin_t pin, aw9523b_pin_mode_t mode);

// LED 模式下设置引脚亮度（0~255 线性恒流，引脚须先切到 LED 模式）
esp_err_t aw9523b_set_led_brightness(aw9523b_pin_t pin, uint8_t dim);

// 全局电流档（CTL[1:0]），影响所有 LED 模式引脚的满量程电流
esp_err_t aw9523b_set_current_range(aw9523b_current_t range);

// P0 口输出结构（CTL.D4；P1 固定推挽不可配）
esp_err_t aw9523b_set_p0_drive_mode(aw9523b_drive_mode_t mode);

/* ── 中断 ────────────────────────────────────────────────────────────────── */
// 端口整写中断屏蔽：mask 位 1=屏蔽 0=使能
esp_err_t aw9523b_set_port_int_mask(aw9523b_port_t port, uint8_t mask);
esp_err_t aw9523b_enable_pin_interrupt (aw9523b_pin_t pin);  // 清屏蔽位
esp_err_t aw9523b_disable_pin_interrupt(aw9523b_pin_t pin);  // 置屏蔽位
esp_err_t aw9523b_clear_interrupt(void);                     // 读两口输入寄存器清 INT

/* ── 调试 ────────────────────────────────────────────────────────────────── */
// 打印两口全部寄存器状态（IN/OUT/CFG/INT/LEDMODE + CTL/ID）
void aw9523b_dump_regs(void);

#endif /* __AW9523B_H__ */
