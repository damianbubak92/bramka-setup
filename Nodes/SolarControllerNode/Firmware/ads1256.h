#include <ti/drivers/GPIO.h>
#include <ti/drivers/SPI.h>
#include <ti/display/Display.h>
#include <unistd.h>
#include "CC1310_LAUNCHXL.h"

#define SPI_MSG_LENGTH  6

unsigned char spiRxBuffer[SPI_MSG_LENGTH];
unsigned char spiTxBuffer[SPI_MSG_LENGTH];

uint32_t Adc_Value[8];

extern Display_Handle display;

SPI_Handle  spi;
SPI_Params  spiParams;
SPI_Transaction transaction;

//uint8_t ScanMode = 0;

/* Gain channel */
typedef enum
{
    ADS1256_GAIN_1          = 0,
    ADS1256_GAIN_2          = 1,
    ADS1256_GAIN_4          = 2,
    ADS1256_GAIN_8          = 3,
    ADS1256_GAIN_16         = 4,
    ADS1256_GAIN_32         = 5,
    ADS1256_GAIN_64         = 6,
}ADS1256_GAIN;

/* Sampling frequency */
typedef enum
{
    ADS1256_30000SPS = 0,
    ADS1256_15000SPS,
    ADS1256_7500SPS,
    ADS1256_3750SPS,
    ADS1256_2000SPS,
    ADS1256_1000SPS,
    ADS1256_500SPS,
    ADS1256_100SPS,
    ADS1256_60SPS,
    ADS1256_50SPS,
    ADS1256_30SPS,
    ADS1256_25SPS,
    ADS1256_15SPS,
    ADS1256_10SPS,
    ADS1256_5SPS,
    ADS1256_2d5SPS,

    ADS1256_DRATE_MAX
}ADS1256_DRATE;

/* Register adresses */
typedef enum
{
    REG_STATUS = 0, // x1H
    REG_MUX    = 1, // 01H
    REG_ADCON  = 2, // 20H
    REG_DRATE  = 3, // F0H
    REG_IO     = 4, // E0H
    REG_OFC0   = 5, // xxH
    REG_OFC1   = 6, // xxH
    REG_OFC2   = 7, // xxH
    REG_FSC0   = 8, // xxH
    REG_FSC1   = 9, // xxH
    REG_FSC2   = 10, // xxH
}ADS1256_REG;

/* Control commands */
typedef enum
{
    CMD_WAKEUP  = 0x00, // Completes SYNC and Exits Standby Mode 0000  0000 (00h)
    CMD_RDATA   = 0x01, // Read Data 0000  0001 (01h)
    CMD_RDATAC  = 0x03, // Read Data Continuously 0000   0011 (03h)
    CMD_SDATAC  = 0x0F, // Stop Read Data Continuously 0000   1111 (0Fh)
    CMD_RREG    = 0x10, // Read from REG rrr 0001 rrrr (1xh)
    CMD_WREG    = 0x50, // Write to REG rrr 0101 rrrr (5xh)
    CMD_SELFCAL = 0xF0, // Offset and Gain Self-Calibration 1111    0000 (F0h)
    CMD_SELFOCAL= 0xF1, // Offset Self-Calibration 1111    0001 (F1h)
    CMD_SELFGCAL= 0xF2, // Gain Self-Calibration 1111    0010 (F2h)
    CMD_SYSOCAL = 0xF3, // System Offset Calibration 1111   0011 (F3h)
    CMD_SYSGCAL = 0xF4, // System Gain Calibration 1111    0100 (F4h)
    CMD_SYNC    = 0xFC, // Synchronize the A/D Conversion 1111   1100 (FCh)
    CMD_STANDBY = 0xFD, // Begin Standby Mode 1111   1101 (FDh)
    CMD_RESET   = 0xFE, // Reset to Power-Up Values 1111   1110 (FEh)
}ADS1256_CMD;



static const uint8_t ADS1256_DRATE_E[ADS1256_DRATE_MAX] =
{
    0xF0,       /*reset the default values  */
    0xE0,
    0xD0,
    0xC0,
    0xB0,
    0xA1,
    0x92,
    0x82,
    0x72,
    0x63,
    0x53,
    0x43,
    0x33,
    0x20,
    0x13,
    0x03
};

uint8_t ADS1256_HardwareInit(uint_least8_t index);
void ADS1256_reset(void);
void ADS1256_WriteCmd(uint8_t Cmd);
void ADS1256_WriteReg(uint8_t Reg, uint8_t data);
uint8_t ADS1256_Read_data(uint8_t Reg);
void ADS1256_WaitDRDY(void);
uint8_t ADS1256_ReadChipID(void);
void ADS1256_ConfigADC(ADS1256_GAIN gain, ADS1256_DRATE drate);
void ADS1256_SetChannel(uint8_t Channel);
uint8_t ADS1256_Init(void);
uint32_t ADS1256_Read_ADC_Data(void);
uint32_t ADS1256_GetChannelValue(uint8_t Channel);
void ADS1256_GetAll(uint32_t *ADC_Value);
void ADS1256_ReadTemperature(float *Temperature);











