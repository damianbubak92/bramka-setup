/*
 * mcp3421.c - MCP3421 18-bit ADC over I2C (see mcp3421.h).
 *
 * Config byte (one-shot, 18-bit, PGA=1):
 *   bit7  RDY  = 1  -> writing 1 in one-shot mode STARTS a conversion;
 *                     reading it back = 0 once the result is ready.
 *   bit6:5 C   = 00 -> channel (single-channel part)
 *   bit4  O/C  = 0  -> one-shot (converts once, then idles = low power)
 *   bit3:2 S   = 11 -> 18-bit / 3.75 SPS
 *   bit1:0 G   = 00 -> PGA x1
 *   => 0x8C
 *
 * 18-bit result: 3 data bytes (two's complement, top byte sign-extended) + the
 * config byte. LSB @ 18-bit/PGA1 = 2*Vref/2^18 = 2*2.048/262144 = 15.625 µV.
 */
#include "mcp3421.h"

#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Clock.h>

#define MCP3421_ADDR        0x68
#define MCP3421_CFG_ONESHOT 0x8C     /* start 18-bit one-shot, PGA x1 */
#define MCP3421_RDY_MASK    0x80     /* config byte: 1 = busy, 0 = ready */
#define MCP3421_LSB_UV      15.625f  /* µV per count, 18-bit, PGA x1 */

static void delay_ms(uint32_t ms) { Task_sleep((ms * 1000UL) / Clock_tickPeriod); }

bool mcp3421_read_uv(I2C_Handle i2c, int32_t *out_uV)
{
    if (i2c == NULL || out_uV == NULL) return false;

    /* Start a one-shot conversion. */
    uint8_t cfg = MCP3421_CFG_ONESHOT;
    I2C_Transaction t;
    t.slaveAddress = MCP3421_ADDR;
    t.writeBuf = &cfg; t.writeCount = 1;
    t.readBuf = NULL;  t.readCount = 0;
    if (!I2C_transfer(i2c, &t)) return false;

    /* Poll until RDY clears (18-bit ~267 ms). Read 4 bytes: 3 data + config. */
    uint8_t rd[4];
    int i;
    for (i = 0; i < 40; i++) {          /* up to ~400 ms */
        delay_ms(10);
        t.slaveAddress = MCP3421_ADDR;
        t.writeBuf = NULL; t.writeCount = 0;
        t.readBuf = rd;    t.readCount = 4;
        if (!I2C_transfer(i2c, &t)) return false;
        if ((rd[3] & MCP3421_RDY_MASK) == 0) {
            /* Sign-extend the 18-bit value: cast the top byte to int8_t (its unused
             * high bits mirror the sign per the datasheet). */
            int32_t raw = ((int32_t)(int8_t)rd[0] << 16) |
                          ((int32_t)rd[1] << 8) | (int32_t)rd[2];
            *out_uV = (int32_t)((float)raw * MCP3421_LSB_UV);
            return true;
        }
    }
    return false;   /* conversion never completed */
}

bool mcp3421_read_batt_mv(I2C_Handle i2c, uint16_t *out_mV, uint8_t samples)
{
    if (out_mV == NULL) return false;
    if (samples < 1)  samples = 1;
    if (samples > 16) samples = 16;

    int64_t acc = 0;
    uint8_t n;
    for (n = 0; n < samples; n++) {
        int32_t uv;
        if (!mcp3421_read_uv(i2c, &uv)) return false;
        acc += uv;
    }
    float vadc_mv = ((float)acc / (float)samples) / 1000.0f;   /* ADC input mV */
    float vbat_mv = vadc_mv * 2.0f * MCP3421_CAL;              /* undo 1:2 divider */
    if (vbat_mv < 0.0f) vbat_mv = 0.0f;
    *out_mV = (uint16_t)(vbat_mv + 0.5f);
    return true;
}
