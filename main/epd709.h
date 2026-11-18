#ifndef __EPD709_H__
#define __EPD709_H__

/* Ported from vendor SampleCodeFor709 / VE071A epd_7in09_e6.
 * Native panel: 1200x1600. Dual IC is LEFT/RIGHT (not top/bottom):
 *   CS0 = nx 0..599, CS1 = nx 600..1199, each 1600 lines x 300 bytes.
 * Product UI is landscape 1600x1200 via 90° CCW (logical lx=ny, ly=1199-nx). */

#define BLACK         0x00
#define WHITE         0x11
#define YELLOW        0x22
#define RED           0x33
#define BLUE          0x55
#define GREEN         0x66

#define PSR             0x00
#define PWR             0x01
#define POF             0x02
#define POFS            0x03
#define PON             0x04
#define BTST_N          0x05
#define BTST_P          0x06
#define DTM             0x10
#define DRF             0x12
#define PLL             0x30
#define TSC             0x40
#define CDI             0x50
#define TCON            0x60
#define TRES            0x61
#define PTLW            0x83
#define AN_TM           0x74
#define AGID            0x86
#define CMDA4           0xA4
#define DCDC            0xA5
#define BUCK_BOOST_VDDN 0xB0
#define TFT_VCOM_POWER  0xB1
#define EN_BUF          0xB6
#define BOOST_VDDP_EN   0xB7
#define CCSET           0xE0
#define PWS             0xE3
#define CMD66           0xF0

#define EPD_HALF_BYTES      480000UL /* per IC: 1600 lines x 300 bytes */
#define EPD_CS_LINE_BYTES   300      /* 600 px / 2 px-per-byte */
#define EPD_CS_LINES        1600
#define EPD_CS_WIDTH_PX     600
#define EPD_LINE_BYTES      600      /* full native width row (1200 px) */
#define EPD_HALF_LINES      800      /* legacy name; prefer EPD_CS_LINES */
#define EPD_NATIVE_W        1200
#define EPD_NATIVE_H        1600
#define EPD_PANEL_W         1600 /* logical landscape width  */
#define EPD_PANEL_H         1200 /* logical landscape height */

#define PTLW_ENABLE  0x01
#define PTLW_DISABLE 0x00

#include "status.h"

#ifdef __EPD709_C__
#define __EPD709_EXTERN__
#else
#define __EPD709_EXTERN__ extern
#endif

__EPD709_EXTERN__ const unsigned char PLL_V[1];
__EPD709_EXTERN__ const unsigned char AN_TM_V[9];
__EPD709_EXTERN__ const unsigned char CMD66_V[6];
__EPD709_EXTERN__ const unsigned char PSR_V[2];
__EPD709_EXTERN__ const unsigned char CDI_V[1];
__EPD709_EXTERN__ const unsigned char TCON_V[2];
__EPD709_EXTERN__ const unsigned char AGID_V[1];
__EPD709_EXTERN__ const unsigned char DCDC_V[3];
__EPD709_EXTERN__ const unsigned char PWS_V[1];
__EPD709_EXTERN__ const unsigned char CCSET_V[1];
__EPD709_EXTERN__ const unsigned char TRES_V[4];
__EPD709_EXTERN__ const unsigned char PWM_V[6];
__EPD709_EXTERN__ const unsigned char EN_BUF_V[1];
__EPD709_EXTERN__ const unsigned char BTST_P_V[2];
__EPD709_EXTERN__ const unsigned char BOOST_VDDP_EN_V[1];
__EPD709_EXTERN__ const unsigned char BTST_N_V[2];
__EPD709_EXTERN__ const unsigned char BUCK_BOOST_VDDN_V[1];
__EPD709_EXTERN__ const unsigned char TFT_VCOM_POWER_V[1];
__EPD709_EXTERN__ const unsigned char POF_V[1];
__EPD709_EXTERN__ const unsigned char POFS_MV[4];
__EPD709_EXTERN__ const unsigned char POFS_SV[4];
__EPD709_EXTERN__ const unsigned char DRF_V[1];
__EPD709_EXTERN__ const unsigned char CMDA4_V[9];

__EPD709_EXTERN__ void epdHardwareReset(void);
__EPD709_EXTERN__ void setPinCsAll(unsigned int setLevel);
__EPD709_EXTERN__ void setPinCs(unsigned char csNumber, unsigned int setLevel);
__EPD709_EXTERN__ void checkBusyHigh(void);
__EPD709_EXTERN__ void checkBusyLow(void);
/* 1 = BUSY reached expected level in time; 0 = timeout (panel may be dirty). */
__EPD709_EXTERN__ int checkBusyHighOk(unsigned int timeout_ms);
__EPD709_EXTERN__ void initEPD(void);
__EPD709_EXTERN__ void writeEpd(unsigned char epdCommand, unsigned char *epdData, unsigned int epdDataLength);
__EPD709_EXTERN__ void readEpd(unsigned char epdCommand, unsigned char *epdData, unsigned int epdDataLength);
__EPD709_EXTERN__ void writeEpdCommand(unsigned char epdCommand);
__EPD709_EXTERN__ void writeEpdData(unsigned char *epdData, unsigned int epdDataLength);
/* Full PON→DRF→POF. Returns 1 on success, 0 if any BUSY wait timed out. */
__EPD709_EXTERN__ int epdDisplay(void);
__EPD709_EXTERN__ unsigned char checkDriverICStatus(void);
__EPD709_EXTERN__ char partialWindowUpdateWithImageData(unsigned char csx, unsigned char const *imageData, unsigned long dataSize, unsigned int xStart, unsigned int yStart, unsigned int xPixel, unsigned int yLine, unsigned char displayEnable);
__EPD709_EXTERN__ char partialWindowUpdateWithoutImageData(unsigned char csx, unsigned int xStart, unsigned int yStart, unsigned int xPixel, unsigned int yLine, unsigned char epdDisplayEnable);
__EPD709_EXTERN__ void executeTscCommand(void);

#endif
