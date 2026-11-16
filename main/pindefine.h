#ifndef __PINDEFINE_H__
#define __PINDEFINE_H__

/*
 * VE071A / EK410 panel wiring (board/VE071A/epd_io_config.h + board_config.h).
 * 7.09" E6 dual-IC, standard SPI (not QSPI). CS naming matches EPD709.c:
 *   SPI_CS0 = master CSB_M, SPI_CS1 = slave CSB_S.
 *
 * Button map (matched on device 2026-11-12):
 *   MAIN     = GPIO2  (short/long software gestures)
 *   PAGE     = GPIO3  (one long rocker — up/down same line)
 *   RESET    = hardware EN/RST (POWERON reboot; not a GPIO)
 *   CHIP BOOT= GPIO9  (strapping only; not a front-panel key)
 */

#define SPI_CS0		23  /* CSB_M, shared with TF via SGM3699 */
#define SPI_CS1		24  /* CSB_S, direct to panel */
#define SPI_CLK		25
#define SPI_MOSI	26
#define SPI_MISO	27

/* Legacy demo names kept for EPD709.c — MOSI/MISO only; no D2/D3. */
#define SPI_Data0	SPI_MOSI
#define SPI_Data1	SPI_MISO

#define EPD_BUSY	29
#define EPD_RST		12
#define EPD_DC		7   /* unused on 7IN09_E6 — hold as Hi-Z input */

#define SPI_CTL		8   /* SGM3699: 1 = EPD, 0 = TF */

#define IOEXT_RST_GPIO	13
#define BOARD_I2C_SDA	22
#define BOARD_I2C_SCL	28
#define AW9523B_I2C_ADDR	0x59

#define BTN_MAIN_GPIO	2
#define BTN_PAGE_GPIO	3
#define BTN_BOOT_GPIO	9   /* chip BOOT strap — not SoftAP */

/* Battery sense (VE071A): VBAT_DECT=GPIO4=ADC1_CH2, enable via AW9523B P0_6. */
#define BAT_ADC_GPIO	4
#define BAT_DIV_MULT	2   /* hardware divider 1/2 */

/* Back-compat alias (older code used AUX for the second key). */
#define BTN_AUX_GPIO	BTN_PAGE_GPIO

#define GPIO_LOW	0
#define GPIO_HIGH	1

#endif
