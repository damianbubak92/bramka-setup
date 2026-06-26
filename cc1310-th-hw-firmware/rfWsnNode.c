/*
 * rfWsnNode.c - gen2 battery T&H node (CC1310 + SHT35 + BQ35100).
 *
 * Power-cycled by a TPL5111 timer: it gates power (TPS22860 + TPS61291) on a
 * fixed (resistor-set) interval; the MCU cold-boots, takes a reading, sends it
 * to the gateway, then asserts DONE (DIO23) so the timer cuts power until the
 * next cycle. Firmware is linear: boot -> measure -> send -> DONE.
 *
 * Bring-up note: this board has NO UART pin (JTAG only), so debug goes to
 * System_printf (visible in CCS via ROV -> SysMin), and the key readings are
 * kept in volatile file-scope vars you can watch in the debugger.
 *
 * NODE_MODE_DONE:
 *   1 = real power-cycle (assert DONE, then halt; timer cuts power).
 *   0 = BENCH/JTAG loop: stay alive, measure+send every NODE_LOOP_S seconds
 *       (debugger stays attached, logs/repeats visible) - isolates firmware
 *       repeatability from the hardware power-cycle.
 */
#include <xdc/std.h>
#include <xdc/runtime/System.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/drivers/GPIO.h>

#include "Board.h"
#include "sensors.h"
#include "radio.h"
#include "node_protocol.h"

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/ioc.h)
#include DeviceFamily_constructPath(driverlib/gpio.h)
#include DeviceFamily_constructPath(driverlib/prcm.h)

#define NODE_MODE_DONE   1          /* 1 = power-cycle (DONE); 0 = bench loop  */
#define NODE_LOOP_S      15         /* bench-loop interval (NODE_MODE_DONE==0) */
#define NODE_ADDRESS     0x1Au      /* fixed for now (provisioning later)      */
#define DONE_DIO         IOID_23    /* TPL5111 DONE feedback line              */

static Task_Struct nodeTaskStruct;
static uint8_t      nodeTaskStack[2048];

/* Watch these in the debugger (no UART on this board). */
static volatile float    g_temp = 0.0f, g_hum = 0.0f;
static volatile uint16_t g_batt_mv = 0;
static volatile bool     g_okTH = false, g_okBatt = false, g_acked = false;
static volatile uint32_t g_cycle = 0;

static void done_init(void)
{
    /* Make sure the GPIO peripheral is clocked (belt + braces alongside
     * GPIO_init()), then hold DONE low so the timer doesn't see a false "done"
     * while we work. (The board also has a 10k pulldown on DONE.) */
    PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
    PRCMLoadSet();
    while (!PRCMLoadGet()) {}
    IOCPinTypeGpioOutput(DONE_DIO);
    GPIO_writeDio(DONE_DIO, 0);
}

static void assert_done(void)
{
    /* TPL5111 DONE idles LOW; a short HIGH *pulse* signals "task complete" (per
     * datasheet) -> the timer deasserts DRV and cuts power. Holding it high is
     * NOT the documented pattern, so we pulse and return to idle low. */
    GPIO_writeDio(DONE_DIO, 0);
    Task_sleep(2000 / Clock_tickPeriod);   /* ~2 ms low baseline   */
    GPIO_writeDio(DONE_DIO, 1);
    Task_sleep(2000 / Clock_tickPeriod);   /* ~2 ms high pulse     */
    GPIO_writeDio(DONE_DIO, 0);            /* back to idle low     */
}

static void measure_and_send(void)
{
    g_cycle++;
    if (!sensors_init()) {
        System_printf("[node] I2C init FAILED\n"); System_flush();
        return;
    }
    g_okTH   = sensors_read_th((float *)&g_temp, (float *)&g_hum);
    g_okBatt = sensors_read_batt_mv((uint16_t *)&g_batt_mv);   /* gauge started+polled in sensors_init */
    sensors_close();

    System_printf("[node] cycle %u  T=%d.%02d C  H=%d.%02d %%  batt=%u mV (th=%d b=%d)\n",
                  g_cycle,
                  (int)g_temp, (int)((g_temp - (int)g_temp) * 100),
                  (int)g_hum,  (int)((g_hum  - (int)g_hum)  * 100),
                  g_batt_mv, g_okTH, g_okBatt);
    System_flush();

    if (g_okTH) {
        MessageStruct msg;
        memset(&msg, 0, sizeof(msg));
        msg.id   = NODE_ADDRESS;
        msg.type = NODE_TH_SENSOR;
        msg.cmd  = CMD_SEND_DATA_TO_DB;
        msg.payload.thData.temperature = g_temp;
        msg.payload.thData.humidity    = g_hum;
        msg.payload.thData.batt_mv     = g_okBatt ? g_batt_mv : 0;
        msg.length = 4 + sizeof(msg.payload.thData);   /* header + thData */

        g_acked = radio_send_message(&msg, ADDR_GATEWAY);
        System_printf("[node] RF -> gateway 0x%02x: acked=%d\n", ADDR_GATEWAY, g_acked);
        System_flush();
    }
}

static void nodeTaskFunction(UArg a0, UArg a1)
{
    done_init();                       /* DONE held low ASAP */

#if NODE_MODE_DONE
    measure_and_send();
    System_printf("[node] asserting DONE (DIO23) -> power off\n"); System_flush();
    assert_done();
    while (1) {}                       /* wait for the timer to cut power */
#else
    /* Bench/JTAG loop: never powers off, repeats so you can watch it live. */
    for (;;) {
        measure_and_send();
        Task_sleep((NODE_LOOP_S * 1000000UL) / Clock_tickPeriod);
    }
#endif
}

int main(void)
{
    Board_initGeneral();
    GPIO_init();                       /* powers the GPIO peripheral (DONE pin) */

    Task_Params params;
    Task_Params_init(&params);
    params.stackSize = sizeof(nodeTaskStack);
    params.priority  = 2;
    params.stack     = nodeTaskStack;
    Task_construct(&nodeTaskStruct, nodeTaskFunction, &params, NULL);

    BIOS_start();
    return (0);
}
