/*
 * epd709.c — ported from vendor SampleCodeFor709_20260129/EPD709.c.
 * Register init sequences, CS handling and refresh flow are unchanged from
 * the vendor demo. Port deltas:
 *  - the 480000-byte epdImageDataBuffer and the two color-fill helpers that
 *    needed it are gone (ESP32-C6 has no RAM for a full frame; app_epd.c
 *    streams chunks instead);
 *  - busy waits are bounded so a stuck panel cannot wedge the RTOS forever.
 */
#define __EPD709_C__

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "EPD709.h"
#include "pindefine.h"
#include "comm.h"
#include "status.h"

static const char *TAG = "epd";

#define BUSY_TIMEOUT_MS         5000
#define BUSY_REFRESH_TIMEOUT_MS 40000 /* 7.09" E6 full refresh can take ~30s */

const unsigned char spiCsPin[2] = {
    SPI_CS0, SPI_CS1
};
const unsigned char PSR_V[2] = { 0xDF, 0x6B };
const unsigned char PWR_V[6] = { 0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38 };
const unsigned char POF_V[1] = { 0x01 };
const unsigned char POFS_MV[4] = { 0x00, 0xC0, 0x03, 0xA8 };
const unsigned char POFS_SV[4] = { 0x00, 0xC0, 0x03, 0x9A };
const unsigned char DRF_V[1] = { 0x00 };
const unsigned char PLL_V[1] = { 0x08 };
const unsigned char CDI_V[1] = { 0x37 };
const unsigned char TCON_V[2] = { 0x03, 0x03 };
const unsigned char TRES_V[4] = { 0x04, 0xB0, 0x03, 0x20 }; /* 1200 x 800 per IC */
const unsigned char CMD66_V[6] = { 0x49, 0x55, 0x13, 0x5D, 0x05, 0x10 };
const unsigned char EN_BUF_V[1] = { 0x07 };
const unsigned char CCSET_V[1] = { 0x01 };
const unsigned char PWS_V[1] = { 0x22 };
const unsigned char AN_TM_V[9] = { 0x00, 0x0C, 0x0C, 0xD9, 0xDD, 0xDD, 0x15, 0x15, 0x55 };
const unsigned char AGID_V[1] = { 0x10 };
const unsigned char CMDA4_V[9] = { 0x03, 0x00, 0x01, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00 };
const unsigned char DCDC_V[3] = { 0x44, 0x54, 0x00 };
const unsigned char BTST_P_V[2] = { 0xE0, 0x20 };
const unsigned char BOOST_VDDP_EN_V[1] = { 0x01 };
const unsigned char BTST_N_V[2] = { 0xE0, 0x20 };
const unsigned char BUCK_BOOST_VDDN_V[1] = { 0x01 };
const unsigned char TFT_VCOM_POWER_V[1] = { 0x02 };

char partialWindowUpdateStatus = DONE;

//================== GPIO Setting ====================================
void resetPin(unsigned int pinStatus)
{
    setGpioLevel(EPD_RST, pinStatus);
}

void setPinCsAll(unsigned int setLevel)
{
    unsigned char i;
    for (i = 0; i < 2; i++) {
        setGpioLevel(spiCsPin[i], setLevel);
    }
}

void setPinCs(unsigned char csNumber, unsigned int setLevel)
{
    setGpioLevel(spiCsPin[csNumber], setLevel);
}

static int waitBusyHigh(unsigned int timeout_ms)
{
    unsigned int waited = 0;
    while (!getGpioLevel(EPD_BUSY)) {
        delayms(10);
        waited += 10;
        if (waited >= timeout_ms) {
            ESP_LOGE(TAG, "busy-high timeout (%u ms)", waited);
            return 0;
        }
    }
    return 1;
}

void checkBusyHigh(void) /* If BUSYN=0 then waiting */
{
    (void)waitBusyHigh(BUSY_TIMEOUT_MS);
}

int checkBusyHighOk(unsigned int timeout_ms)
{
    return waitBusyHigh(timeout_ms ? timeout_ms : BUSY_TIMEOUT_MS);
}

void checkBusyLow(void) /* If BUSYN=1 then waiting */
{
    unsigned int waited = 0;
    while (getGpioLevel(EPD_BUSY)) {
        delayms(10);
        waited += 10;
        if (waited >= BUSY_TIMEOUT_MS) {
            ESP_LOGE(TAG, "busy-low timeout (%u ms)", waited);
            return;
        }
    }
}
//====================================================================

void epdHardwareReset(void)
{
    resetPin(GPIO_LOW);
    delayms(20);
    resetPin(GPIO_HIGH);
    delayms(20);
}

void writeEpd(unsigned char epdCommand, unsigned char *epdData, unsigned int epdDataLength)
{
    spiTransmit(epdCommand, epdData, epdDataLength);
}

void readEpd(unsigned char epdCommand, unsigned char *epdData, unsigned int epdDataLength)
{
    spiReceive(epdCommand, epdData, epdDataLength);
}

void writeEpdCommand(unsigned char epdCommand)
{
    spiTransmitCommand(epdCommand);
}

void writeEpdData(unsigned char *epdData, unsigned int epdDataLength)
{
    spiTransmitData(epdData, epdDataLength);
}

void initEPD(void)
{
    epdHardwareReset();
    checkBusyHigh();

    setPinCs(0, GPIO_LOW);
    writeEpd(AN_TM, AN_TM_V, sizeof(AN_TM_V));
    setPinCsAll(GPIO_HIGH);

    setPinCsAll(GPIO_LOW);
    writeEpd(CMD66, CMD66_V, sizeof(CMD66_V));
    setPinCsAll(GPIO_HIGH);

    setPinCsAll(GPIO_LOW);
    writeEpd(PSR, PSR_V, sizeof(PSR_V));
    setPinCsAll(GPIO_HIGH);

    setPinCs(0, GPIO_LOW);
    writeEpd(DCDC, DCDC_V, sizeof(DCDC_V));
    setPinCsAll(GPIO_HIGH);

    setPinCsAll(GPIO_LOW);
    writeEpd(PLL, PLL_V, sizeof(PLL_V));
    setPinCsAll(GPIO_HIGH);

    setPinCsAll(GPIO_LOW);
    writeEpd(CDI, CDI_V, sizeof(CDI_V));
    setPinCsAll(GPIO_HIGH);

    setPinCsAll(GPIO_LOW);
    writeEpd(TCON, TCON_V, sizeof(TCON_V));
    setPinCsAll(GPIO_HIGH);

    setPinCs(0, GPIO_LOW);
    writeEpd(POFS, POFS_MV, sizeof(POFS_MV));
    setPinCsAll(GPIO_HIGH);

    setPinCs(1, GPIO_LOW);
    writeEpd(POFS, POFS_SV, sizeof(POFS_SV));
    setPinCsAll(GPIO_HIGH);

    setPinCsAll(GPIO_LOW);
    writeEpd(AGID, AGID_V, sizeof(AGID_V));
    setPinCsAll(GPIO_HIGH);

    setPinCsAll(GPIO_LOW);
    writeEpd(PWS, PWS_V, sizeof(PWS_V));
    setPinCsAll(GPIO_HIGH);

    setPinCsAll(GPIO_LOW);
    writeEpd(CCSET, CCSET_V, sizeof(CCSET_V));
    setPinCsAll(GPIO_HIGH);

    setPinCsAll(GPIO_LOW);
    writeEpd(TRES, TRES_V, sizeof(TRES_V));
    setPinCsAll(GPIO_HIGH);

    setPinCs(0, GPIO_LOW);
    writeEpd(CMDA4, CMDA4_V, sizeof(CMDA4_V));
    setPinCsAll(GPIO_HIGH);

    setPinCs(0, GPIO_LOW);
    writeEpd(PWR, PWR_V, sizeof(PWR_V));
    setPinCsAll(GPIO_HIGH);

    setPinCs(0, GPIO_LOW);
    writeEpd(EN_BUF, EN_BUF_V, sizeof(EN_BUF_V));
    setPinCsAll(GPIO_HIGH);

    setPinCs(0, GPIO_LOW);
    writeEpd(BTST_P, BTST_P_V, sizeof(BTST_P_V));
    setPinCsAll(GPIO_HIGH);

    setPinCs(0, GPIO_LOW);
    writeEpd(BOOST_VDDP_EN, BOOST_VDDP_EN_V, sizeof(BOOST_VDDP_EN_V));
    setPinCsAll(GPIO_HIGH);

    setPinCs(0, GPIO_LOW);
    writeEpd(BTST_N, BTST_N_V, sizeof(BTST_N_V));
    setPinCsAll(GPIO_HIGH);

    setPinCs(0, GPIO_LOW);
    writeEpd(BUCK_BOOST_VDDN, BUCK_BOOST_VDDN_V, sizeof(BUCK_BOOST_VDDN_V));
    setPinCsAll(GPIO_HIGH);

    setPinCs(0, GPIO_LOW);
    writeEpd(TFT_VCOM_POWER, TFT_VCOM_POWER_V, sizeof(TFT_VCOM_POWER_V));
    setPinCsAll(GPIO_HIGH);

#if SHOW_LOG
    printf("initEPD() has been executed. \r\n");
#endif
}

unsigned char checkDriverICStatus(void)
{
    unsigned char csx, status = DONE;
    unsigned char dataBuf[3];

    for (csx = 0; csx < 2; csx++) {
        memset(dataBuf, 0, sizeof(dataBuf));
        setPinCs(csx, GPIO_LOW);
        readEpd(0xF2, dataBuf, sizeof(dataBuf));
        setPinCs(csx, GPIO_HIGH);
#if SHOW_LOG
        printf("Driver IC [%d] = 0x%02X 0x%02X 0x%02X \r\n", csx, dataBuf[0], dataBuf[1], dataBuf[2]);
#endif
        if ((dataBuf[0] & 0x01) == 0x01) {
#if SHOW_LOG
            printf("Driver IC [%d] is ready. \r\n", csx);
#endif
        } else {
#if SHOW_LOG
            printf("Driver IC [%d] did not reply. \r\n", csx);
#endif
            status = ERROR;
        }
    }
    return status;
}

int epdDisplay(void)
{
    int ok = 1;

    setPinCsAll(GPIO_LOW);
    writeEpdCommand(PON);
    if (!waitBusyHigh(BUSY_TIMEOUT_MS)) {
        ok = 0;
    }
    setPinCsAll(GPIO_HIGH);

    setPinCsAll(GPIO_LOW);
    delayms(30);
    writeEpd(DRF, DRF_V, sizeof(DRF_V));
    if (!waitBusyHigh(BUSY_REFRESH_TIMEOUT_MS)) {
        ok = 0;
    }
    setPinCsAll(GPIO_HIGH);

    setPinCsAll(GPIO_LOW);
    writeEpd(POF, POF_V, sizeof(POF_V));
    if (!waitBusyHigh(15000)) { /* POF can lag after a long refresh */
        ok = 0;
    }
    setPinCsAll(GPIO_HIGH);
    /* Ensure BUSY is idle before the next frame writer starts. */
    if (!waitBusyHigh(BUSY_TIMEOUT_MS)) {
        ok = 0;
    }
    delayms(80);
#if SHOW_LOG
    if (ok) {
        printf("Display Done!! \r\n");
    } else {
        printf("Display incomplete (BUSY timeout)!! \r\n");
    }
#endif
    return ok;
}

void executeTscCommand(void)
{
    unsigned char readTscBuf[2];
    /* Read TSC */
    setPinCs(0, GPIO_LOW);
    spiTransmitCommand(TSC);
    checkBusyHigh();
    spiReceiveData(&readTscBuf[0], sizeof(readTscBuf));
    setPinCs(0, GPIO_HIGH);
    delayms(30);
#if SHOW_LOG
    printf("TSC Data = 0x%02X, 0x%02X \r\n", readTscBuf[0], readTscBuf[1]);
#endif
}

void writeEpdImage(unsigned char csx, unsigned char const *imageData, unsigned long imageDataLength)
{
    if (csx == 0) {
        executeTscCommand();
    }
    setPinCs(csx, GPIO_LOW);
    spiTransmitLargeData(DTM, (unsigned char *)imageData, imageDataLength);
    setPinCs(csx, GPIO_HIGH);
#if SHOW_LOG
    printf("Writing data is completed. \r\n");
#endif
}

char partialWindowUpdateWithImageData(unsigned char csx, unsigned char const *imageData, unsigned long dataSize,
                                      unsigned int xStart, unsigned int yStart, unsigned int xPixel, unsigned int yLine,
                                      unsigned char displayEnable)
{
    unsigned char status = DONE;
    unsigned int HRST, HRED, VRST, VRED;
    unsigned char partialWindowData[9];

    HRST = xStart * 2;
    HRED = (xStart + xPixel) * 2 - 1;
    VRST = yStart / 2;
    VRED = (yStart + yLine) / 2 - 1;

    if (HRST % 8 != 0) {
        status = -1;
    } else if ((HRED - 7) % 8 != 0) {
        status = -2;
    } else if ((xStart > 584) | (xPixel > 600)) {
        status = -3;
    } else if ((HRED - HRST + 1 < 32) | (HRED + 1 > 1200)) {
        status = -4;
    } else if ((yStart + yLine) % 2 != 0) {
        status = -5;
    } else if ((yStart > 1596) | (yLine > 1600)) {
        status = -6;
    } else if (((int)(VRED - VRST) + 1 <= 0) | (VRED + 1 > 800)) {
        status = -7;
    } else if (csx > 1) {
        status = -8;
    } else {
        memset(partialWindowData, 0, sizeof(partialWindowData));
        partialWindowData[0] = (unsigned char)(HRST >> 8);
        partialWindowData[1] = (unsigned char)(HRST);
        partialWindowData[2] = (unsigned char)(HRED >> 8);
        partialWindowData[3] = (unsigned char)(HRED);
        partialWindowData[4] = (unsigned char)(VRST >> 8);
        partialWindowData[5] = (unsigned char)(VRST);
        partialWindowData[6] = (unsigned char)(VRED >> 8);
        partialWindowData[7] = (unsigned char)(VRED);
        partialWindowData[8] = PTLW_ENABLE;

        if (csx == 0) {
            executeTscCommand();
        }
        setPinCs(csx, GPIO_LOW);
        writeEpd(PTLW, partialWindowData, sizeof(partialWindowData));
        setPinCs(csx, GPIO_HIGH);

        setPinCs(csx, GPIO_LOW);
        spiTransmitLargeData(DTM, (unsigned char *)imageData, dataSize);
        setPinCs(csx, GPIO_HIGH);
    }

    if (status != DONE) {
        partialWindowUpdateStatus = ERROR;
    }

    if (displayEnable) {
        if (partialWindowUpdateStatus == DONE) {
            epdDisplay();
        }
        delayms(300);
        memset(partialWindowData, 0, sizeof(partialWindowData));
        partialWindowData[8] = PTLW_DISABLE;
        partialWindowUpdateStatus = DONE;
        setPinCsAll(GPIO_LOW);
        writeEpd(PTLW, partialWindowData, sizeof(partialWindowData));
        setPinCsAll(GPIO_HIGH);
    }
    return status;
}

char partialWindowUpdateWithoutImageData(unsigned char csx, unsigned int xStart, unsigned int yStart,
                                         unsigned int xPixel, unsigned int yLine, unsigned char epdDisplayEnable)
{
    return partialWindowUpdateWithImageData(csx, NULL, 0, xStart, yStart, xPixel, yLine, epdDisplayEnable);
}
