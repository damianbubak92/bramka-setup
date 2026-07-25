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
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/drivers/GPIO.h>
#include <ti/drivers/UART.h>
#include <ti/drivers/NVS.h>

#include "Board.h"
#include "sensors.h"
#include "radio.h"
#include "node_protocol.h"

#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(driverlib/ioc.h)
#include DeviceFamily_constructPath(driverlib/gpio.h)
#include DeviceFamily_constructPath(driverlib/prcm.h)
#include DeviceFamily_constructPath(inc/hw_types.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_fcfg1.h)
#include <ti/drivers/GPIO.h>

/* rev-2 bring-up SWEEP: 1 = drive each IOID 0..30 HIGH for 3 s in turn (skips JTAG
 * DIO16/17). Keep a multimeter on the pad in question (e.g. package pin 18 = UART_TX);
 * when it reads 3.3 V, HALT the debugger and read `g_sweep_ioid` = the real IOID on
 * that pad. Use it to fix the pin map (the KiCad symbol's DIO names are wrong). */
#define NODE_PIN_SWEEP_TEST   0

/* rev-2 bring-up pin check: 1 = toggle NODE_TOGGLE_IOID 2 s HIGH / 2 s LOW on pin 18. */
#define NODE_PIN_TOGGLE_TEST  0
/* <<< SET THIS to the g_sweep_ioid value that drove pin 18 to 3.3 V >>> */
#define NODE_TOGGLE_IOID      12

/* rev-2 bring-up: 1 = ONLY prove the UART talks (TX=DIO12), print a counter every
 * 1 s and loop forever - nothing else runs. Set 0 once UART is confirmed. */
#define NODE_UART_SMOKE_TEST  0

/* rev-2 bring-up: 1 = press JOIN (DIO26) -> send a JOIN_REQUEST to the gateway over
 * radio (factory_id from FCFG, capabilities=0), log to UART. */
#define NODE_JOIN_TEST        1

/* rev-2 low-power telemetry cadence (provisioned steady state). */
#define NODE_SLEEP_S     240u   /* deep-sleep (STANDBY ~1uA) between cycles = 4 min */
#define NODE_BATT_EVERY  5u     /* read MCP3421 battery every Nth cycle (SoC moves slowly) */
#define NODE_DEBUG_UART  1      /* 1 = per-cycle UART log (opened+closed each cycle so STANDBY still works); 0 = silent, lowest power */

/* JOIN provisioning window: after a button press the node stays AWAKE (radio in RX,
 * ~6 mA) up to NODE_JOIN_WINDOW_S waiting for JOIN_ACCEPT. You must approve in the app
 * within this window or the node sleeps again (press JOIN to retry). */
#define NODE_JOIN_WINDOW_S   90u

#define NODE_MODE_DONE   0          /* 1 = power-cycle (DONE); 0 = bench loop  */
#define NODE_LOOP_S      15         /* bench-loop interval (NODE_MODE_DONE==0) */
#define NODE_ADDRESS     0x1Au      /* fixed for now (provisioning later)      */
#define DONE_DIO         IOID_23    /* TPL5111 DONE feedback line              */

static Task_Struct nodeTaskStruct;
static uint8_t      nodeTaskStack[2048];

/* Watch these in the debugger (no UART on this board). */
static volatile float    g_temp = 0.0f, g_hum = 0.0f;
static volatile uint16_t g_batt_mv = 0;
static volatile uint8_t  g_soh = 0;
static volatile int32_t  g_acc_uah = 0;   /* cumulative used uAh (ACC mode) - consumption measurement */
static volatile bool     g_okTH = false, g_okBatt = false, g_okSoh = false, g_okAcc = false, g_acked = false;
static volatile uint32_t g_cycle = 0;
static volatile int      g_sweep_ioid = -1;   /* rev-2 sweep: IOID currently driven HIGH (WATCH this) */
static volatile bool     g_joinPressed = false;

/* JOIN button wake: the GPIO interrupt (AON) wakes the MCU from STANDBY and posts this,
 * so a press is handled immediately even mid-sleep (not only at the next 4-min cycle). */
static Semaphore_Struct  joinSemStruct;
static Semaphore_Handle  joinSem;

/* Read the chip's IEEE MAC from FCFG = the immutable factory id (8 B, little-endian). */
static void read_factory_id(uint8_t out[NODE_FACTORY_ID_LEN])
{
    uint32_t lo = HWREG(FCFG1_BASE + FCFG1_O_MAC_15_4_0);
    uint32_t hi = HWREG(FCFG1_BASE + FCFG1_O_MAC_15_4_1);
    out[0] = (uint8_t)(lo);       out[1] = (uint8_t)(lo >> 8);
    out[2] = (uint8_t)(lo >> 16); out[3] = (uint8_t)(lo >> 24);
    out[4] = (uint8_t)(hi);       out[5] = (uint8_t)(hi >> 8);
    out[6] = (uint8_t)(hi >> 16); out[7] = (uint8_t)(hi >> 24);
}

/* JOIN button ISR-context callback: flag it AND post the wake semaphore so the task
 * blocked in Semaphore_pend wakes from STANDBY and provisions right away. Semaphore_post
 * is ISR-safe in SYS/BIOS. */
static void joinButtonCb(uint_least8_t index)
{
    (void)index;
    g_joinPressed = true;
    Semaphore_post(joinSem);
}

/* ---- persisted RF address (internal-flash NVS, Board_NVSINTERNAL) ---- */
#define TH_IDENT_MAGIC  0x54484E31u   /* "THN1" - guards uninitialized flash */
typedef struct { uint32_t magic; uint8_t addr; uint8_t _pad[3]; } ThIdentRec;

/* Returns the persisted RF address, or ADDR_UNPROVISIONED (0xFF) if none/invalid. */
static uint8_t nvs_load_address(void)
{
    NVS_Params p;
    NVS_init();
    NVS_Params_init(&p);
    NVS_Handle h = NVS_open(Board_NVSINTERNAL, &p);
    if (h == NULL) return ADDR_UNPROVISIONED;

    ThIdentRec rec;
    uint8_t addr = ADDR_UNPROVISIONED;
    if (NVS_read(h, 0, &rec, sizeof(rec)) == NVS_STATUS_SUCCESS &&
        rec.magic == TH_IDENT_MAGIC &&
        rec.addr >= ADDR_POOL_FIRST && rec.addr <= ADDR_POOL_LAST) {
        addr = rec.addr;
    }
    NVS_close(h);
    return addr;
}

/* Persist the gateway-assigned RF address so a power-cycle skips re-JOIN. */
static bool nvs_save_address(uint8_t addr)
{
    NVS_Params p;
    NVS_init();
    NVS_Params_init(&p);
    NVS_Handle h = NVS_open(Board_NVSINTERNAL, &p);
    if (h == NULL) return false;

    ThIdentRec rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = TH_IDENT_MAGIC;
    rec.addr  = addr;
    int rc = NVS_write(h, 0, &rec, sizeof(rec), NVS_WRITE_ERASE | NVS_WRITE_POST_VERIFY);
    NVS_close(h);
    return (rc == NVS_STATUS_SUCCESS);
}

/* Open UART, printf ONE line, close it - releasing the UART power dependency so STANDBY
 * still works around it. NODE_DEBUG_UART 0 compiles this to a no-op (lowest power). */
#if NODE_DEBUG_UART
static void dbg_uart(const char *fmt, ...)
{
    UART_Params up;
    UART_Params_init(&up);
    up.baudRate      = 115200;
    up.writeDataMode = UART_DATA_BINARY;
    UART_Handle u = UART_open(Board_UART0, &up);
    if (u == NULL) return;

    char b[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    if (n > 0) UART_write(u, b, (size_t)n);
    UART_close(u);
}
#else
static inline void dbg_uart(const char *fmt, ...) { (void)fmt; }
#endif

/* JOIN button = Board_GPIO_BUTTON1 (rev-2: DIO26), active low. The interrupt wakes the
 * MCU from STANDBY (AON) and posts joinSem. Set up ONCE before streaming - and in BOTH
 * paths (fresh JOIN and NVS-provisioned), so a provisioned node can still re-JOIN. */
static void setup_join_button(void)
{
    GPIO_setConfig(Board_GPIO_BUTTON1, GPIO_CFG_IN_PU | GPIO_CFG_IN_INT_FALLING);
    GPIO_setCallback(Board_GPIO_BUTTON1, joinButtonCb);
    GPIO_enableInt(Board_GPIO_BUTTON1);
}

/* Send one JOIN_REQUEST and listen up to 60 s for JOIN_ACCEPT. On accept, persist the
 * gateway-assigned address to NVS and return it in *addrOut. Returns true ONLY on a real
 * accept: an already-active node is silenced by the gateway (returns false -> keep addr);
 * a removed/detached node surfaces in the app for re-pair. Reused for first JOIN + re-JOIN. */
static bool provision_join(const uint8_t *factoryId, uint8_t *addrOut)
{
    MessageStruct msg;
    memset(&msg, 0, sizeof(msg));
    msg.id   = ADDR_UNPROVISIONED;               /* 0xFF = unprovisioned source */
    msg.type = NODE_TH_SENSOR;
    msg.cmd  = CMD_JOIN_REQUEST;
    memcpy(msg.payload.joinData.factory_id, factoryId, NODE_FACTORY_ID_LEN);
    msg.payload.joinData.capabilities = 0;       /* sensor: no actions */
    msg.length = (uint8_t)(4 + sizeof(msg.payload.joinData));

    /* One JOIN, then listen the whole window for JOIN_ACCEPT. We listen even if the
     * JOIN wasn't RF-ACKed (the gateway may have received it but the ACK back was lost),
     * to maximise the chance of catching the accept. If none arrives, sleep; the user
     * presses JOIN again to retry. */
    bool acked = radio_send_message(&msg, ADDR_GATEWAY, NULL, NULL);  /* JOIN = legacy 'D' */
    dbg_uart("[node] JOIN sent -> gw 0x%02x acked=%d - %us window, approve on the phone...\r\n",
             ADDR_GATEWAY, acked, NODE_JOIN_WINDOW_S);

    uint8_t addr = 0;
    if (radio_wait_join_accept(factoryId, &addr, NODE_JOIN_WINDOW_S * 1000u)) {
        /* Node got the address AND radio_wait_join_accept RF-ACKed it to the gateway;
         * now persist to NVS. The gateway only marks us `active` once our FIRST telemetry
         * arrives under this address (MarkActive) - that is the real "provisioning done". */
        bool saved = nvs_save_address(addr);     /* persist so a reboot skips JOIN */
        *addrOut = addr;
        dbg_uart("[node] *** JOIN_ACCEPT *** addr 0x%02x saved=%d - streaming\r\n", addr, saved);
        return true;
    }
    dbg_uart("[node] no JOIN_ACCEPT in %us - back to sleep (press JOIN to retry)\r\n", NODE_JOIN_WINDOW_S);
    return false;
}

/* Provisioned steady state (low-power): each cycle powers the SHT35 rail (PERIPH_EN),
 * opens I2C, reads T/RH (+ battery every Nth), closes everything, sends telemetry, then
 * deep-sleeps NODE_SLEEP_S. All peripherals are released before the sleep so the CC1310
 * enters STANDBY (~1 uA) between cycles. The sleep is a Semaphore_pend, so a JOIN button
 * press wakes it EARLY (GPIO wake) and (re-)provisions on the spot.
 * Returns true if the gateway UNREGISTERED us (§14: a NACK on the telemetry ACK window) -
 * NVS is wiped and the caller drops back to the unprovisioned shelf; otherwise never returns. */
static bool stream_telemetry(uint8_t addr, const uint8_t *factoryId)
{
    uint16_t sBattMv  = 0;
    uint8_t  sBattCtr = 0;

    for (;;) {
        /* --- wake window: power sensors, measure, power down --- */
        GPIO_write(Board_PERIPH_EN, 1);              /* SHT35 rail + ADC divider on */
        Task_sleep((20000UL) / Clock_tickPeriod);    /* settle */
        sensors_init();                              /* I2C open */

        float t = 0.0f, h = 0.0f;
        bool okth = sensors_read_th(&t, &h);

        uint16_t mv = sBattMv;
        if (sBattCtr == 0) {                         /* battery only every Nth cycle */
            if (sensors_read_mcp3421_mv(&mv)) sBattMv = mv;
        }
        sBattCtr = (uint8_t)((sBattCtr + 1) % NODE_BATT_EVERY);

        sensors_close();                             /* I2C close -> release dependency */
        GPIO_write(Board_PERIPH_EN, 0);              /* rail + divider off */

        bool charging = (GPIO_read(Board_nCHRGSTAT) == 0);

        /* --- send (radio_send_message opens+closes RF itself) --- */
        MessageStruct tm;
        memset(&tm, 0, sizeof(tm));
        tm.id   = addr;
        tm.type = NODE_TH_SENSOR;
        tm.cmd  = CMD_SEND_DATA_TO_DB;
        tm.payload.thData.temperature = t;
        tm.payload.thData.humidity    = h;
        tm.payload.thData.batt_mv     = sBattMv;
        tm.length = (uint8_t)(4 + sizeof(tm.payload.thData));
        /* 'E' frame (factory_id) so the gateway can key its §14 pending-command table.
         * The gateway may reply with a NACK carrying a command instead of an ACK. */
        uint8_t nackCmd = 0;
        bool tacked = okth ? radio_send_message(&tm, ADDR_GATEWAY, factoryId, &nackCmd) : false;

        dbg_uart("[node] addr=0x%02x  T=%d.%02d C  H=%d.%02d %%  batt=%u mV chg=%d  sht=%d acked=%d  -> sleep %us (or JOIN)\r\n",
                 addr, (int)t, (int)((t - (int)t) * 100),
                 (int)h, (int)((h - (int)h) * 100), sBattMv, charging, okth, tacked, NODE_SLEEP_S);

        /* §14: gateway piggybacked UNREGISTERED/REMOVE on the ACK window = "clear your
         * identity, go silent". Obey: echo a best-effort confirm (cmd == delivered cmd, so
         * the CC1310 recognises it and drops the entry), wipe NVS, return -> caller re-enters
         * the unprovisioned shelf. Idempotent: if the confirm is lost we're silent anyway. */
        if (nackCmd == CMD_UNREGISTERED || nackCmd == CMD_REMOVE) {
            MessageStruct cf;
            memset(&cf, 0, sizeof(cf));
            cf.id     = addr;
            cf.type   = NODE_TH_SENSOR;
            cf.cmd    = nackCmd;                  /* echo the delivered command = the confirm */
            cf.length = 4u;                       /* header only */
            (void)radio_send_message(&cf, ADDR_GATEWAY, factoryId, NULL);
            (void)nvs_save_address(ADDR_UNPROVISIONED);   /* out of pool -> loads as unprovisioned */
            dbg_uart("[node] UNREGISTERED by gateway (cmd %u) - NVS cleared, going to shelf\r\n", nackCmd);
            return true;
        }

        /* --- deep sleep: STANDBY (~1 uA) until the next cycle (RTC) OR a JOIN press
         * (GPIO wake posts joinSem). On a press, (re-)provision: an accept switches to the
         * new address; no accept (already-active node silenced by gw) keeps the old one. --- */
        Semaphore_pend(joinSem, (NODE_SLEEP_S * 1000000UL) / Clock_tickPeriod);
        if (g_joinPressed) {
            g_joinPressed = false;
            uint8_t newAddr;
            if (provision_join(factoryId, &newAddr)) {
                addr = newAddr;                      /* re-provisioned (possibly new address) */
            }
        }
    }
}

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

/* Read sensors + battery accumulator and send one telemetry frame. Assumes I2C
 * is open and a gauging session is already active (caller owns GAUGE_START/STOP). */
static void read_and_send(void)
{
    g_cycle++;
    g_okAcc  = sensors_read_used_uah((int32_t *)&g_acc_uah);  /* cumulative used uAh (continuous session) */
    sensors_read_opconfig((uint8_t *)&g_soh);                 /* DIAG: OpConfig A (GMSEL=bits[1:0]) -> soh_pct */
    g_okTH   = sensors_read_th((float *)&g_temp, (float *)&g_hum);
    g_okBatt = sensors_read_batt_mv((uint16_t *)&g_batt_mv);

    System_printf("[node] cycle %u  T=%d.%02d C  H=%d.%02d %%  batt=%u mV  usedUAh=%ld  opcfg=0x%02x  (th=%d b=%d a=%d)\n",
                  g_cycle,
                  (int)g_temp, (int)((g_temp - (int)g_temp) * 100),
                  (int)g_hum,  (int)((g_hum  - (int)g_hum)  * 100),
                  g_batt_mv, (long)g_acc_uah, g_soh, g_okTH, g_okBatt, g_okAcc);
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
        msg.payload.thData.soh_pct     = g_soh;                        /* DIAG: OpConfig A byte */
        msg.payload.thData.acc_uah     = g_okAcc ? g_acc_uah : 0;      /* cumulative used uAh */
        msg.length = 4 + sizeof(msg.payload.thData);

        g_acked = radio_send_message(&msg, ADDR_GATEWAY, NULL, NULL);  /* legacy bench path ('D') */
        System_printf("[node] RF -> gateway 0x%02x: acked=%d\n", ADDR_GATEWAY, g_acked);
        System_flush();
    }
}

static void nodeTaskFunction(UArg a0, UArg a1)
{
#if NODE_PIN_SWEEP_TEST
    /* --- rev-2 SWEEP: drive each IOID HIGH 3 s in turn; find which one reaches pin 18 ---
     * Keep the multimeter on pin 18; when it reads 3.3 V, HALT and read g_sweep_ioid. */
    PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
    PRCMLoadSet();
    while (!PRCMLoadGet()) {}
    System_printf("[sweep] driving each IOID HIGH 3s - watch pin 18, halt when 3.3V, read g_sweep_ioid\n"); System_flush();
    for (;;) {
        int io;
        for (io = 0; io <= 30; io++) {
            if (io == 16 || io == 17) continue;        /* skip JTAG TDO/TDI (would drop the debugger) */
            g_sweep_ioid = io;
            System_printf("[sweep] IOID_%d HIGH\n", io); System_flush();
            IOCPinTypeGpioOutput((uint32_t)io);
            GPIO_writeDio((uint32_t)io, 1);
            Task_sleep((3000000UL) / Clock_tickPeriod);
            GPIO_writeDio((uint32_t)io, 0);
            IOCPinTypeGpioInput((uint32_t)io);         /* release before next */
        }
    }
#endif

#if NODE_PIN_TOGGLE_TEST
    /* --- rev-2 pin check: toggle NODE_TOGGLE_IOID 2 s HIGH / 2 s LOW, measure pin 18 ---
     * Power the GPIO peripheral FIRST (as done_init does). */
    PRCMPeripheralRunEnable(PRCM_PERIPH_GPIO);
    PRCMLoadSet();
    while (!PRCMLoadGet()) {}
    System_printf("[pin-test] toggling IOID_%d 2s/2s - measure pin 18\n", NODE_TOGGLE_IOID); System_flush();
    IOCPinTypeGpioOutput(NODE_TOGGLE_IOID);
    for (;;) {
        GPIO_writeDio(NODE_TOGGLE_IOID, 1);             /* pin 18 -> 3.3 V */
        Task_sleep((2000000UL) / Clock_tickPeriod);
        GPIO_writeDio(NODE_TOGGLE_IOID, 0);             /* pin 18 -> 0 V   */
        Task_sleep((2000000UL) / Clock_tickPeriod);
    }
#endif

#if NODE_UART_SMOKE_TEST
    /* --- rev-2 UART smoke test: prove TX on DIO12 ---
     * Diagnostics go to System_printf (SysMin) too, so you can tell CONFIG failure
     * from WIRING failure via the debugger (CCS: Tools -> ROV -> SysMin), which works
     * over JTAG regardless of the UART wire. */
    UART_Params up;
    UART_init();
    UART_Params_init(&up);
    up.baudRate      = 115200;
    up.writeDataMode = UART_DATA_BINARY;   /* we add \r\n ourselves */
    System_printf("[uart-test] opening UART0 (TX must be DIO12)...\n"); System_flush();
    UART_Handle uart = UART_open(Board_UART0, &up);
    System_printf("[uart-test] UART_open -> %s\n", uart ? "OK" : "NULL=FAILED"); System_flush();
    if (uart == NULL) {
        while (1) {}                       /* CONFIG problem - see SysMin line above  */
    }
    {
        const char banner[] =
            "\r\n===================================\r\n"
            " rev-2 T&H node - UART ALIVE @115200\r\n"
            " TX on DIO12 (pkg pin 18). Hello!\r\n"
            "===================================\r\n";
        UART_write(uart, banner, sizeof(banner) - 1);

        uint32_t i = 0;
        char buf[64];
        for (;;) {
            int n = snprintf(buf, sizeof(buf), "[rev2 T&H] uart tick #%lu  (jesli to widzisz - dziala!)\r\n",
                             (unsigned long)i++);
            if (n > 0) {
                UART_write(uart, buf, (size_t)n);          /* -> physical TX (DIO12)   */
            }
            Task_sleep((1000000UL) / Clock_tickPeriod);    /* ~1 s                     */
        }
    }
#endif

#if NODE_JOIN_TEST
    /* --- rev-2 provisioning: wake-on-button JOIN + NVS address + low-power streaming ---
     * A JOIN press wakes the MCU from STANDBY and provisions, whether the node is fresh
     * (shelf) or already provisioned (re-JOIN). No 100 ms polling: the node sleeps until
     * the button (or the 4-min telemetry cycle) via Semaphore_pend. */
    {
        UART_init();                               /* dbg_uart opens/closes per line */

        uint8_t factoryId[NODE_FACTORY_ID_LEN];
        read_factory_id(factoryId);

        /* Wake semaphore + JOIN button (both paths, so a provisioned node can re-JOIN). */
        Semaphore_Params semP;
        Semaphore_Params_init(&semP);
        semP.mode = Semaphore_Mode_BINARY;         /* one pending post is enough */
        Semaphore_construct(&joinSemStruct, 0, &semP);
        joinSem = Semaphore_handle(&joinSemStruct);
        setup_join_button();

        /* Outer loop: after the gateway UNREGISTERS us (§14), stream_telemetry wipes NVS
         * and returns -> we re-read NVS (now unprovisioned) and drop back to the shelf. */
        for (;;) {
        uint8_t savedAddr = nvs_load_address();
        if (savedAddr == ADDR_UNPROVISIONED) {
            /* Shelf state: sleep in STANDBY until JOIN is pressed, then provision. */
            dbg_uart("\r\n[node] UNPROVISIONED factory=%02x%02x%02x%02x%02x%02x%02x%02x"
                     " - press JOIN (DIO26) to provision via gateway 0x%02x\r\n",
                     factoryId[0], factoryId[1], factoryId[2], factoryId[3],
                     factoryId[4], factoryId[5], factoryId[6], factoryId[7], ADDR_GATEWAY);
            for (;;) {
                Semaphore_pend(joinSem, BIOS_WAIT_FOREVER);   /* STANDBY until the button */
                if (g_joinPressed) {
                    g_joinPressed = false;
                    uint8_t addr;
                    if (provision_join(factoryId, &addr)) { savedAddr = addr; break; }
                }
            }
        } else {
            dbg_uart("\r\n[node] provisioned from NVS: address 0x%02x - streaming"
                     " (press JOIN to re-provision)\r\n", savedAddr);
        }

        (void)stream_telemetry(savedAddr, factoryId);  /* returns only on UNREGISTERED -> re-enter shelf */
        }  /* outer for(;;) */
    }
#endif

    done_init();                       /* DONE held low ASAP */

    if (!sensors_init()) {
        System_printf("[node] I2C init FAILED\n"); System_flush();
    }
    bool comm = sensors_commission();  /* ensure ACC mode + design cap (idempotent) */
    System_printf("[node] commission=%d\n", comm); System_flush();

#if NODE_MODE_DONE
    /* Production-style: one session brackets the work, then power off. (Note: with
     * per-cycle power-off the gauge can't accumulate across cycles - that's why the
     * measurement uses bench mode below.) */
    sensors_gauge_begin();
    read_and_send();
    sensors_gauge_end();
    sensors_close();
    System_printf("[node] asserting DONE (DIO23) -> power off\n"); System_flush();
    assert_done();
    while (1) {}                       /* wait for the timer to cut power */
#else
    /* MEASUREMENT (bench, continuous power on battery, NO JTAG): ONE gauging
     * session for the whole run, so AccumulatedCapacity accumulates continuously.
     * acc_uah should climb steadily -> average current = d(acc_uah)/d(time).
     * Per-loop GAUGE_START/STOP would reset the accumulator each loop (the bug). */
    sensors_gauge_begin();             /* GAUGE_START once - never stop */
    for (;;) {
        read_and_send();
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
