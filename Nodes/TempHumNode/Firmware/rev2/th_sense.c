/*
 * th_sense.c - SHT35 + MCP3421 sensing facade (see th_sense.h).
 *
 * SHT35 read is the proven rev-1 code (sensors.c). Battery is MCP3421 (mcp3421.c)
 * + the LFP SoC LUT (battery_soc.c). Both share one I2C_Handle; PERIPH_EN gates the
 * SHT35 rail and the ADC divider.
 *
 * ⚠️ BOARD PINS (rev-2 schematic, VERIFY against the final layout / SysConfig):
 *   Board_PERIPH_EN  = DIO5   (out, drives TPS22860 + divider MOSFET)
 *   Board_nCHRGSTAT  = DIO7   (in,  MCP73123 STAT: low = charging)
 *   Board_I2C0       = SDA/SCL shared bus (SHT35 0x45, MCP3421 0x68)
 * Map these in the board file / SysConfig. If bench-testing on a LaunchXL without
 * the PERIPH_EN rail, build with -DTH_NO_PERIPH_EN (calls become no-ops).
 */
#include "th_sense.h"
#include "mcp3421.h"
#include "battery_soc.h"

#include <ti/drivers/I2C.h>
#include <ti/drivers/GPIO.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Clock.h>
#include "Board.h"

/* SHT35 (I2C addr set by the ADDR strap: 0x44 low / 0x45 high). rev-1 used 0x45. */
#define SHT35_ADDR      0x45
#define SHT35_CMD_MSB   0x2C   /* clock-stretch, high repeatability */
#define SHT35_CMD_LSB   0x06

#define PERIPH_SETTLE_MS  20   /* SHT35 power-up + divider RC settle */
#define BATT_SAMPLES       4   /* MCP3421 averages per battery read */

static I2C_Handle i2c = NULL;

static void delay_ms(uint32_t ms) { Task_sleep((ms * 1000UL) / Clock_tickPeriod); }

bool th_sense_init(void)
{
    I2C_Params params;
    I2C_init();
    I2C_Params_init(&params);
    params.bitRate = I2C_100kHz;
    i2c = I2C_open(Board_I2C0, &params);
    if (i2c == NULL) return false;

#ifndef TH_NO_PERIPH_EN
    GPIO_init();   /* idempotent - safe even if main already called it */
    GPIO_setConfig(Board_PERIPH_EN, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_LOW);
    GPIO_setConfig(Board_nCHRGSTAT, GPIO_CFG_IN_PU);
#endif
    return true;
}

void th_sense_close(void)
{
    if (i2c != NULL) { I2C_close(i2c); i2c = NULL; }
}

void th_sense_periph(bool on)
{
#ifndef TH_NO_PERIPH_EN
    GPIO_write(Board_PERIPH_EN, on ? 1 : 0);
#else
    (void)on;
#endif
}

void th_sense_settle(void)
{
    delay_ms(PERIPH_SETTLE_MS);
}

bool th_sense_read_th(float *tempC, float *rh)
{
    if (i2c == NULL || tempC == NULL || rh == NULL) return false;
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

bool th_sense_read_batt(uint16_t *mv, uint8_t *soc_pct, bool *low)
{
    if (mv == NULL) return false;
    uint16_t m = 0;
    if (!mcp3421_read_batt_mv(i2c, &m, BATT_SAMPLES)) return false;
    *mv = m;
    if (soc_pct != NULL) *soc_pct = battery_soc_pct(m);
    if (low != NULL)     *low     = battery_is_low(m);
    return true;
}

bool th_sense_charging(void)
{
#ifndef TH_NO_PERIPH_EN
    return GPIO_read(Board_nCHRGSTAT) == 0;   /* STAT open-drain: low = charging */
#else
    return false;
#endif
}
