/*
 * sensors.c - SHT35 (temp/hum) + BQ35100 (battery) over I2C.
 *
 * BQ35100 sequence follows the u-blox driver (the device is sequence-sensitive):
 *   GE high (here: hard-wired to VCC_SWITCH, so high whenever the MCU is awake)
 *   -> settle -> GAUGE_START via ManufacturerAccessControl (0x3E <- 0x0011)
 *   -> poll CONTROL_STATUS (0x00) bit0 (GA) until 1 -> read Voltage() (0x08).
 * Voltage() is only valid once gauging is ACTIVE (GA=1).
 */
#include "sensors.h"

#include <ti/drivers/I2C.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Clock.h>
#include "Board.h"

#define SHT35_I2C_ADDRESS    0x45
#define SHT35_CMD_MSB        0x2C   /* high-rep, clock-stretching */
#define SHT35_CMD_LSB        0x06

#define BQ35100_I2C_ADDRESS  0x55
#define BQ35100_REG_CONTROL  0x00   /* Control() write / CONTROL_STATUS read */
#define BQ35100_REG_VOLTAGE  0x08   /* Voltage(), 2 B, mV, LSB-first          */
#define BQ35100_REG_MAC      0x3E   /* ManufacturerAccessControl              */
#define BQ_SUBCMD_GAUGE_START 0x0011
#define BQ_GE_SETTLE_MS      50     /* after GE rises (rail up) before talking */
#define BQ_GA_POLL_TRIES     100    /* x10 ms = up to ~1 s for GA bit          */

static I2C_Handle i2c = NULL;

static void delay_ms(uint32_t ms)
{
    Task_sleep((ms * 1000UL) / Clock_tickPeriod);
}

/* Write a 16-bit subcommand (little-endian) to a BQ35100 command register. */
static bool bq_write_subcmd(uint8_t reg, uint16_t sub)
{
    uint8_t b[3] = { reg, (uint8_t)(sub & 0xFF), (uint8_t)(sub >> 8) };
    I2C_Transaction t;
    t.slaveAddress = BQ35100_I2C_ADDRESS;
    t.writeBuf = b; t.writeCount = 3;
    t.readBuf = NULL; t.readCount = 0;
    return I2C_transfer(i2c, &t);
}

/* Read a 2-byte LSB-first register from the BQ35100. */
static bool bq_read_u16(uint8_t reg, uint16_t *out)
{
    uint8_t r = reg, d[2];
    I2C_Transaction t;
    t.slaveAddress = BQ35100_I2C_ADDRESS;
    t.writeBuf = &r; t.writeCount = 1;
    t.readBuf = d;  t.readCount = 2;
    if (!I2C_transfer(i2c, &t)) return false;
    *out = ((uint16_t)d[1] << 8) | d[0];
    return true;
}

/* GAUGE_START then wait for CONTROL_STATUS GA bit (bit0) = 1. */
static bool bq_gauge_start(void)
{
    if (!bq_write_subcmd(BQ35100_REG_MAC, BQ_SUBCMD_GAUGE_START)) return false;
    int i;
    for (i = 0; i < BQ_GA_POLL_TRIES; i++) {
        uint16_t cs;
        if (bq_read_u16(BQ35100_REG_CONTROL, &cs) && (cs & 0x0001)) return true;
        delay_ms(10);
    }
    return false;   /* GA never set -> gauge not active */
}

bool sensors_init(void)
{
    I2C_Params params;
    I2C_init();
    I2C_Params_init(&params);
    params.bitRate = I2C_100kHz;
    i2c = I2C_open(Board_I2C0, &params);
    if (i2c == NULL) return false;

    delay_ms(BQ_GE_SETTLE_MS);   /* let the BQ35100 settle after GE rose */
    bq_gauge_start();            /* best-effort: voltage read fails gracefully if it didn't */
    return true;
}

void sensors_close(void)
{
    if (i2c != NULL) {
        I2C_close(i2c);
        i2c = NULL;
    }
}

bool sensors_read_th(float *tempC, float *rh)
{
    if (i2c == NULL) return false;

    uint8_t wr[2] = { SHT35_CMD_MSB, SHT35_CMD_LSB };
    uint8_t rd[6];
    I2C_Transaction t;
    t.slaveAddress = SHT35_I2C_ADDRESS;
    t.writeBuf  = wr;  t.writeCount = 2;
    t.readBuf   = rd;  t.readCount  = 6;

    if (!I2C_transfer(i2c, &t)) return false;

    uint16_t rawT = ((uint16_t)rd[0] << 8) | rd[1];   /* rd[2] = CRC */
    uint16_t rawH = ((uint16_t)rd[3] << 8) | rd[4];   /* rd[5] = CRC */
    *tempC = -45.0f + 175.0f * ((float)rawT / 65535.0f);
    *rh    = 100.0f * ((float)rawH / 65535.0f);
    return true;
}

bool sensors_read_batt_mv(uint16_t *mv)
{
    if (i2c == NULL) return false;
    return bq_read_u16(BQ35100_REG_VOLTAGE, mv);   /* gauge started in sensors_init */
}
