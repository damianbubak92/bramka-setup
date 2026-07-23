/*
 * sensors.c - SHT35 (temp/hum) + BQ35100 (battery, SOH mode) over I2C.
 *
 * BQ35100 sequence/constants from the Pavel Slama mbed BQ35100 library + u-blox
 * driver. Li-MnO2 (CR123A) -> SOH_MODE (GMSEL 0b01). The gauge is always-on (VDD
 * direct to battery); GE = VCC_SWITCH so it's enabled during the awake window.
 */
#include "sensors.h"

#include <ti/drivers/I2C.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Clock.h>
#include "Board.h"

/* SHT35 */
#define SHT35_ADDR           0x45
#define SHT35_CMD_MSB        0x2C
#define SHT35_CMD_LSB        0x06

/* BQ35100 */
#define BQ_ADDR              0x55
#define BQ_CMD_CONTROL       0x00   /* read = CONTROL_STATUS                  */
#define BQ_CMD_ACC_CAP       0x02   /* AccumulatedCapacity, 4 B, uAh (ACC mode) */
#define BQ_CMD_VOLTAGE       0x08
#define BQ_CMD_SOH           0x2E   /* StateOfHealth, 1 B, %                  */
#define BQ_CMD_DESIGN_CAP    0x3C   /* DesignCapacity, 2 B, mAh              */
#define BQ_CMD_MAC           0x3E   /* ManufacturerAccessControl             */
#define BQ_CMD_MAC_DATA      0x40
#define BQ_CMD_MAC_DATA_SUM  0x60
#define BQ_CMD_MAC_DATA_LEN  0x61
/* control subcommands */
#define BQ_SUB_GAUGE_START   0x0011
#define BQ_SUB_GAUGE_STOP    0x0012
#define BQ_SUB_NEW_BATTERY   0xA613
/* CONTROL_STATUS bit masks */
#define BQ_GA_MASK           0x0001
#define BQ_GDONE_MASK        0x0040
#define BQ_FLASHF_MASK       0x8000
/* data-flash addresses */
#define BQ_DF_OPCONFIG_A     0x41B1   /* GMSEL in bits[1:0]                  */
#define BQ_DF_DESIGN_CAP     0x41FE   /* [hi, lo] mAh                        */
/* gauge mode (GMSEL bits[1:0] in Operation Config A) */
#define BQ_GMODE_ACC         0x00     /* ACCUMULATOR mode (coulomb) - for consumption measurement */
/* security */
#define BQ_SEALED            0x03     /* (CONTROL_STATUS >> 13) & 0x03       */
#define BQ_SEAL_KEY1_HI      0x04
#define BQ_SEAL_KEY1_LO      0x14
#define BQ_SEAL_KEY2_HI      0x36
#define BQ_SEAL_KEY2_LO      0x72

#define GE_SETTLE_MS         50
#define DESIGN_CAP_MAH       1500   /* Panasonic CR123A */

static I2C_Handle i2c = NULL;

static void delay_ms(uint32_t ms) { Task_sleep((ms * 1000UL) / Clock_tickPeriod); }

static bool bq_cmd_write(uint8_t reg, const uint8_t *data, uint8_t len)
{
    uint8_t b[1 + 36];
    uint8_t i;
    b[0] = reg;
    for (i = 0; i < len; i++) b[1 + i] = data[i];
    I2C_Transaction t;
    t.slaveAddress = BQ_ADDR;
    t.writeBuf = b; t.writeCount = (size_t)(1 + len);
    t.readBuf = NULL; t.readCount = 0;
    return I2C_transfer(i2c, &t);
}

static bool bq_cmd_read(uint8_t reg, uint8_t *buf, uint8_t len)
{
    I2C_Transaction t;
    t.slaveAddress = BQ_ADDR;
    t.writeBuf = &reg; t.writeCount = 1;
    t.readBuf = buf;   t.readCount = len;
    return I2C_transfer(i2c, &t);
}

static bool bq_read_u16(uint8_t reg, uint16_t *out)
{
    uint8_t d[2];
    if (!bq_cmd_read(reg, d, 2)) return false;
    *out = ((uint16_t)d[1] << 8) | d[0];   /* LSB-first */
    return true;
}

static bool bq_subcmd(uint16_t sub)
{
    uint8_t b[2] = { (uint8_t)(sub & 0xFF), (uint8_t)(sub >> 8) };  /* LSB-first */
    return bq_cmd_write(BQ_CMD_MAC, b, 2);
}

/* DataFlash write: MAC block [addr_lo,addr_hi,data...] -> MACDataSum -> MACDataLen. */
static bool bq_df_write(uint16_t addr, const uint8_t *data, uint8_t len)
{
    uint8_t blk[2 + 32];
    uint8_t i, cks = 0;
    blk[0] = (uint8_t)(addr & 0xFF);
    blk[1] = (uint8_t)(addr >> 8);
    for (i = 0; i < len; i++) blk[2 + i] = data[i];
    if (!bq_cmd_write(BQ_CMD_MAC, blk, (uint8_t)(2 + len))) return false;
    for (i = 0; i < 2 + len; i++) cks += blk[i];
    cks = (uint8_t)(0xFF - cks);
    if (!bq_cmd_write(BQ_CMD_MAC_DATA_SUM, &cks, 1)) return false;
    uint8_t maclen = (uint8_t)(4 + len);
    if (!bq_cmd_write(BQ_CMD_MAC_DATA_LEN, &maclen, 1)) return false;
    delay_ms(20);
    uint16_t st;
    if (bq_read_u16(BQ_CMD_CONTROL, &st) && (st & BQ_FLASHF_MASK)) return false;  /* flash write fail */
    return true;
}

/* DataFlash read: set MAC address, read MACData. */
static bool bq_df_read(uint16_t addr, uint8_t *buf, uint8_t len)
{
    uint8_t a[2] = { (uint8_t)(addr & 0xFF), (uint8_t)(addr >> 8) };
    if (!bq_cmd_write(BQ_CMD_MAC, a, 2)) return false;
    delay_ms(5);
    return bq_cmd_read(BQ_CMD_MAC_DATA, buf, len);
}

bool sensors_init(void)
{
    I2C_Params params;
    I2C_init();
    I2C_Params_init(&params);
    params.bitRate = I2C_100kHz;
    i2c = I2C_open(Board_I2C0, &params);
    if (i2c == NULL) return false;
    delay_ms(GE_SETTLE_MS);   /* BQ35100 settle after GE (VCC_SWITCH) rose */
    return true;
}

void sensors_close(void)
{
    if (i2c != NULL) { I2C_close(i2c); i2c = NULL; }
}

bool sensors_commission(void)
{
    uint16_t dcap = 0;
    uint8_t  oc = 0;
    /* Idempotent: configured for ACCUMULATOR mode + design cap already? */
    bool capOk  = bq_read_u16(BQ_CMD_DESIGN_CAP, &dcap) && dcap == DESIGN_CAP_MAH;
    bool modeOk = bq_df_read(BQ_DF_OPCONFIG_A, &oc, 1) && (oc & 0x03) == BQ_GMODE_ACC;
    if (capOk && modeOk) return true;

    /* Unseal if sealed (default keys). */
    uint16_t st = 0;
    bq_read_u16(BQ_CMD_CONTROL, &st);
    if (((st >> 13) & 0x03) == BQ_SEALED) {
        uint8_t k1[2] = { BQ_SEAL_KEY1_HI, BQ_SEAL_KEY1_LO };
        uint8_t k2[2] = { BQ_SEAL_KEY2_HI, BQ_SEAL_KEY2_LO };
        bq_cmd_write(BQ_CMD_MAC, k1, 2);
        bq_cmd_write(BQ_CMD_MAC, k2, 2);
        delay_ms(20);
    }

    /* Gauge mode = ACCUMULATOR (GMSEL bits[1:0] = 00) in Operation Config A. */
    if (bq_df_read(BQ_DF_OPCONFIG_A, &oc, 1)) {
        oc = (uint8_t)(oc & ~0x03);   /* 00 = ACC */
        bq_df_write(BQ_DF_OPCONFIG_A, &oc, 1);
    }

    /* Design capacity (mAh), DF stored [hi, lo]. */
    uint8_t dc[2] = { (uint8_t)(DESIGN_CAP_MAH >> 8), (uint8_t)(DESIGN_CAP_MAH & 0xFF) };
    bq_df_write(BQ_DF_DESIGN_CAP, dc, 2);

    /* NOTE: deliberately NO NEW_BATTERY here - it resets the accumulator, and if
     * the idempotency check ever fails we'd zero it every cycle. We measure via
     * deltas between cycles, so an absolute zero start is not needed. */

    capOk  = bq_read_u16(BQ_CMD_DESIGN_CAP, &dcap) && dcap == DESIGN_CAP_MAH;
    modeOk = bq_df_read(BQ_DF_OPCONFIG_A, &oc, 1) && (oc & 0x03) == BQ_GMODE_ACC;
    return (capOk && modeOk);
}

bool sensors_gauge_begin(void)
{
    bq_subcmd(BQ_SUB_GAUGE_START);
    int i;
    for (i = 0; i < 100; i++) {   /* wait GA=1, up to ~1 s */
        uint16_t s;
        if (bq_read_u16(BQ_CMD_CONTROL, &s) && (s & BQ_GA_MASK)) return true;
        delay_ms(10);
    }
    return false;
}

void sensors_gauge_end(void)
{
    bq_subcmd(BQ_SUB_GAUGE_STOP);
    int i;
    for (i = 0; i < 200; i++) {   /* wait G_DONE, up to ~2 s (SOH stores to NVM) */
        uint16_t s;
        if (bq_read_u16(BQ_CMD_CONTROL, &s) && (s & BQ_GDONE_MASK)) break;
        delay_ms(10);
    }
}

bool sensors_read_th(float *tempC, float *rh)
{
    if (i2c == NULL) return false;
    uint8_t wr[2] = { SHT35_CMD_MSB, SHT35_CMD_LSB };
    uint8_t rd[6];
    I2C_Transaction t;
    t.slaveAddress = SHT35_ADDR;
    t.writeBuf = wr; t.writeCount = 2;
    t.readBuf = rd;  t.readCount = 6;
    if (!I2C_transfer(i2c, &t)) return false;
    uint16_t rawT = ((uint16_t)rd[0] << 8) | rd[1];
    uint16_t rawH = ((uint16_t)rd[3] << 8) | rd[4];
    *tempC = -45.0f + 175.0f * ((float)rawT / 65535.0f);
    *rh    = 100.0f * ((float)rawH / 65535.0f);
    return true;
}

bool sensors_read_batt_mv(uint16_t *mv)
{
    if (i2c == NULL) return false;
    return bq_read_u16(BQ_CMD_VOLTAGE, mv);
}

bool sensors_read_soh(uint8_t *soh_pct)
{
    if (i2c == NULL) return false;
    return bq_cmd_read(BQ_CMD_SOH, soh_pct, 1);
}

/* DIAGNOSTIC: raw Operation Config A byte (GMSEL = bits[1:0]; 00=ACC, 01=SOH). */
bool sensors_read_opconfig(uint8_t *oc)
{
    if (i2c == NULL) return false;
    return bq_df_read(BQ_DF_OPCONFIG_A, oc, 1);
}

/* AccumulatedCapacity (ACC mode): the register counts negative as charge leaves
 * the cell, so used uAh = -(signed raw). Returns cumulative used. */
bool sensors_read_used_uah(int32_t *used_uah)
{
    if (i2c == NULL) return false;
    uint8_t d[4];
    if (!bq_cmd_read(BQ_CMD_ACC_CAP, d, 4)) return false;
    uint32_t raw = (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
                   ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
    *used_uah = -(int32_t)raw;   /* discharge -> raw negative -> used positive */
    return true;
}

/* ---- rev-2 battery: MCP3421 (18-bit dS ADC, I2C 0x68) via a 1:2 divider ----
 * Vbat = 2 * Vadc (* cal). One-shot 18-bit: config 0x8C, LSB 15.625 uV @ PGA1.
 * Divider + MCP3421 are on the PERIPH_EN rail (must be enabled by the caller). */
#define MCP3421_ADDR         0x68
#define MCP3421_CFG_ONESHOT  0x8C
#define MCP3421_RDY_MASK     0x80
#define MCP3421_LSB_UV       15.625f
#define MCP3421_DIVIDER      2.0f
#ifndef MCP3421_CAL
/* 1-point cal: corrects the systematic gain error from the MCP3421 input impedance
 * loading the high-Z 1M/1M divider (~500k source). Calibrated 2026-07-23: node read
 * 2594 mV, multimeter 3268 mV -> 3268/2594 = 1.2598. (rev-3: use a lower-impedance
 * divider or a buffer to shrink this correction + its temp/part sensitivity.) */
#define MCP3421_CAL          1.2598f
#endif

bool sensors_read_mcp3421_mv(uint16_t *mv)
{
    if (i2c == NULL || mv == NULL) return false;

    /* start a one-shot conversion */
    uint8_t cfg = MCP3421_CFG_ONESHOT;
    I2C_Transaction t;
    t.slaveAddress = MCP3421_ADDR;
    t.writeBuf = &cfg; t.writeCount = 1;
    t.readBuf = NULL;  t.readCount = 0;
    if (!I2C_transfer(i2c, &t)) return false;

    uint8_t rd[4];
    int i;
    for (i = 0; i < 40; i++) {              /* poll RDY, up to ~400 ms (18-bit ~267 ms) */
        delay_ms(10);
        t.slaveAddress = MCP3421_ADDR;
        t.writeBuf = NULL; t.writeCount = 0;
        t.readBuf = rd;    t.readCount = 4;
        if (!I2C_transfer(i2c, &t)) return false;
        if ((rd[3] & MCP3421_RDY_MASK) == 0) {
            int32_t raw = ((int32_t)(int8_t)rd[0] << 16) |
                          ((int32_t)rd[1] << 8) | (int32_t)rd[2];
            float vin_mv = (float)raw * MCP3421_LSB_UV / 1000.0f;   /* ADC input mV */
            float vbat   = vin_mv * MCP3421_DIVIDER * MCP3421_CAL;  /* undo 1:2      */
            if (vbat < 0.0f) vbat = 0.0f;
            *mv = (uint16_t)(vbat + 0.5f);
            return true;
        }
    }
    return false;   /* conversion never completed */
}
