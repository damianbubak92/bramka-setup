/*
 * M4F baseline firmware template - working RPMsg echo + graceful shutdown.
 *
 * This is a reference implementation showing:
 *  - Asynchronous RPMsg communication with Linux via callback pattern
 *  - Periodic autonomous work on M4F (1Hz tick counter)
 *  - Command parsing (ping, status, generic echo)
 *  - GRACEFUL SHUTDOWN HANDLER (required for hot-reload workflow!)
 *
 * Build with TI MCU+ SDK 12.x for AM62X, M4F core, noRTOS.
 *
 * After building, deploy via:
 *   scp Debug/<project>.out root@<bramka>:/tmp/my_fw.out
 *   ssh root@<bramka> "m4f-reload"
 *
 * For background and design notes, see ../../docs/M4F_SHUTDOWN.md
 */

#include <stdio.h>
#include <string.h>
#include <kernel/dpl/ClockP.h>
#include <kernel/dpl/DebugP.h>
#include <kernel/dpl/HwiP.h>
#include <kernel/dpl/SystemP.h>
#include <drivers/ipc_notify.h>
#include <drivers/ipc_rpmsg.h>
#include "ti_drivers_open_close.h"
#include "ti_board_open_close.h"

/* === Configuration === */

/* Linux chrdev expects endpoint 14 (must match SysConfig and Linux side) */
#define LINUX_CHRDEV_ENDPOINT       (14U)
#define LINUX_CHRDEV_SERVICE        "rpmsg_chrdev"
#define MAX_MSG_SIZE                (256u)
#define LINUX_CORE_ID               CSL_CORE_ID_A53SS0_0


/* === Global state === */

/* RPMessage object - must be global, persisted across function calls */
static RPMessage_Object gRecvMsgObject;

/* Buffer for incoming Linux message - written by RPMsg callback (ISR context),
 * read by main loop. Mark volatile to prevent compiler caching.
 */
static volatile struct {
    char     data[MAX_MSG_SIZE];
    uint16_t dataLen;
    uint16_t remoteCoreId;
    uint16_t remoteEndPt;
    volatile uint8_t pending;  /* 1 = new message ready, 0 = empty */
} gRxBuffer;

/* Tick counter (autonomous heartbeat) */
static uint32_t gTickCount = 0;
static uint64_t gLastTickTime = 0;

/* Shutdown state - set by callback when Linux requests shutdown */
static volatile uint8_t gbShutdown = 0u;
static volatile uint8_t gbShutdownRemotecoreID = 0u;


/* === Callbacks (ISR context - keep them short!) === */

/*
 * RPMessage callback - called when Linux sends a message to our endpoint.
 * Runs in interrupt/task context. Copy data and signal main loop.
 */
static void linuxMsgCallback(RPMessage_Object *obj, void *arg,
                              void *data, uint16_t dataLen,
                              uint16_t remoteCoreId, uint16_t remoteEndPt)
{
    /* Drop new message if previous still unprocessed (simple back-pressure).
     * For production, implement a proper queue.
     */
    if (gRxBuffer.pending) {
        DebugP_log("[M4F] WARNING: dropped message, previous still pending\r\n");
        return;
    }

    if (dataLen > MAX_MSG_SIZE - 1) {
        dataLen = MAX_MSG_SIZE - 1;
    }

    memcpy((void *)gRxBuffer.data, data, dataLen);
    gRxBuffer.data[dataLen] = 0;  /* null terminator for printf safety */
    gRxBuffer.dataLen = dataLen;
    gRxBuffer.remoteCoreId = remoteCoreId;
    gRxBuffer.remoteEndPt = remoteEndPt;

    /* Atomic flag set - signal to main loop */
    gRxBuffer.pending = 1;
}


/*
 * IPC Notify callback for Linux remoteproc messages.
 * Linux sends IPC_NOTIFY_RP_MBOX_SHUTDOWN when 'echo stop > .../state' is run.
 * We must respond with IPC_NOTIFY_RP_MBOX_SHUTDOWN_ACK or Linux times out.
 */
static void ipc_rp_mbox_callback(uint16_t remoteCoreId, uint16_t clientId,
                                  uint32_t msgValue, void *args)
{
    if (clientId == IPC_NOTIFY_CLIENT_ID_RP_MBOX) {
        if (msgValue == IPC_NOTIFY_RP_MBOX_SHUTDOWN) {
            gbShutdown = 1u;
            gbShutdownRemotecoreID = (uint8_t)remoteCoreId;
            /* No RPMessage_unblock needed here - we use callback-based
             * receive (linuxMsgCallback), not blocking RPMessage_recv.
             * Main loop checks gbShutdown directly and exits.
             */
        }
    }
}


/* === Application logic === */

/*
 * Handle a Linux message that arrived via callback.
 * Called from main loop when gRxBuffer.pending == 1.
 */
static void handleLinuxMessage(void)
{
    char replyMsg[MAX_MSG_SIZE + 32];
    int32_t status;

    DebugP_log("[M4F] Received from Linux (%d bytes): '%s'\r\n",
                gRxBuffer.dataLen, gRxBuffer.data);

    /* Simple command parsing - extend as needed */
    if (strncmp((const char *)gRxBuffer.data, "ping", 4) == 0) {
        snprintf(replyMsg, sizeof(replyMsg), "pong from M4F (tick=%u)",
                  (unsigned int)gTickCount);
    }
    else if (strncmp((const char *)gRxBuffer.data, "status", 6) == 0) {
        snprintf(replyMsg, sizeof(replyMsg), "M4F status: tick=%u, uptime_ms=%llu",
                  (unsigned int)gTickCount,
                  (unsigned long long)(ClockP_getTimeUsec() / 1000));
    }
    else {
        snprintf(replyMsg, sizeof(replyMsg), "M4F got: %s", gRxBuffer.data);
    }

    /* Reply to the same endpoint that sent the message */
    status = RPMessage_send(replyMsg, strlen(replyMsg) + 1,
                             gRxBuffer.remoteCoreId, gRxBuffer.remoteEndPt,
                             RPMessage_getLocalEndPt(&gRecvMsgObject),
                             1000);  /* 1s timeout */

    if (status != SystemP_SUCCESS) {
        DebugP_log("[M4F] Send reply failed: %d\r\n", status);
    }

    /* Clear flag - ready for next message */
    gRxBuffer.pending = 0;
}


/*
 * Periodic autonomous action - 1 Hz heartbeat tick.
 * Demonstrates M4F doing work independently of Linux requests.
 *
 * In production, replace tick logging with:
 *   - Sensor sampling
 *   - State machine updates
 *   - Control loop iterations
 *   - Periodic status reports to Linux via RPMessage_send
 */
static void doPeriodicTick(void)
{
    uint64_t now = ClockP_getTimeUsec();

    if ((now - gLastTickTime) >= 1000000ULL) {  /* 1 second elapsed */
        gTickCount++;
        gLastTickTime = now;

        DebugP_log("[M4F] Tick #%u\r\n", (unsigned int)gTickCount);

        /* Example: spontaneous server-push to Linux (uncomment when Linux
         * service is listening on /dev/rpmsg* to consume tick messages).
         *
         * char tickMsg[64];
         * int len = snprintf(tickMsg, sizeof(tickMsg),
         *                     "tick %u", (unsigned int)gTickCount);
         * RPMessage_send(tickMsg, len + 1,
         *                LINUX_CORE_ID, LINUX_CHRDEV_ENDPOINT,
         *                RPMessage_getLocalEndPt(&gRecvMsgObject),
         *                100);
         */
    }
}


/*
 * Graceful shutdown sequence.
 *
 * CRITICAL: order matters. System_deinit() MUST come BEFORE IpcNotify_sendMsg
 * with the shutdown ACK. Empirically verified - sending ACK before System_deinit
 * causes Linux to time out waiting (the ACK goes into a buffer that deinit
 * clears or reroutes).
 *
 * Order matches official SDK example: examples/drivers/ipc/ipc_rpmsg_echo_linux/
 *
 * This function never returns - CPU halts in WFI loop.
 */
static void doShutdown(void)
{
    DebugP_log("[M4F] Shutdown requested by core %u\r\n",
                (unsigned int)gbShutdownRemotecoreID);
    DebugP_log("[M4F] Closing drivers, deinit system, sending ACK\r\n");

    /* Step 1: close all opened drivers (UART, GPIO, peripherals) */
    Drivers_close();

    /* Step 2: deinit system (disables interrupts, stops tick timer).
     * MUST be before ACK send - see comment above.
     */
    System_deinit();

    /* Step 3: send ACK to Linux remoteproc driver.
     * Linux is waiting for this to mark M4F as 'offline'.
     */
    if (gbShutdownRemotecoreID != 0) {
        IpcNotify_sendMsg(gbShutdownRemotecoreID,
                          IPC_NOTIFY_CLIENT_ID_RP_MBOX,
                          IPC_NOTIFY_RP_MBOX_SHUTDOWN_ACK,
                          1u);
    }

    /* Step 4: halt CPU - wait for interrupt that never arrives.
     * Linux will load fresh firmware on next 'echo start > .../state'.
     */
    while (1) {
        __asm__ __volatile__ ("wfi" "\n\t": : : "memory");
    }
}


/* === Main entry point (called by SDK after init) === */

void ipc_rpmsg_echo_main(void *args)
{
    int32_t status;
    RPMessage_CreateParams createParams;

    DebugP_log("[M4F] Application starting...\r\n");

    /* Initialize state */
    gRxBuffer.pending = 0;

    /* Wait for Linux to be ready before any IPC operations */
    DebugP_log("[M4F] Waiting for Linux ready...\r\n");
    status = RPMessage_waitForLinuxReady(SystemP_WAIT_FOREVER);
    DebugP_assert(status == SystemP_SUCCESS);
    DebugP_log("[M4F] Linux ready!\r\n");

    /* Create RPMsg endpoint with async callback for incoming messages */
    RPMessage_CreateParams_init(&createParams);
    createParams.localEndPt = LINUX_CHRDEV_ENDPOINT;
    createParams.recvCallback = linuxMsgCallback;
    createParams.recvCallbackArgs = NULL;

    status = RPMessage_construct(&gRecvMsgObject, &createParams);
    DebugP_assert(status == SystemP_SUCCESS);

    /* Announce endpoint to Linux so it can find us */
    status = RPMessage_announce(LINUX_CORE_ID, LINUX_CHRDEV_ENDPOINT,
                                  LINUX_CHRDEV_SERVICE);
    DebugP_assert(status == SystemP_SUCCESS);
    DebugP_log("[M4F] Endpoint '%s' announced on EP %d\r\n",
                LINUX_CHRDEV_SERVICE, LINUX_CHRDEV_ENDPOINT);

    /* Register shutdown handler - CRITICAL for hot-reload workflow */
    status = IpcNotify_registerClient(IPC_NOTIFY_CLIENT_ID_RP_MBOX,
                                       ipc_rp_mbox_callback,
                                       NULL);
    DebugP_assert(status == SystemP_SUCCESS);
    DebugP_log("[M4F] Shutdown handler registered\r\n");

    DebugP_log("[M4F] Ready for messages\r\n");

    gLastTickTime = ClockP_getTimeUsec();

    /* Main cooperative loop - run until Linux requests shutdown */
    while (!gbShutdown) {
        /* Process any incoming Linux message */
        if (gRxBuffer.pending) {
            handleLinuxMessage();
        }

        /* Run periodic autonomous work */
        doPeriodicTick();

        /* Add your other periodic work here:
         *   sample_sensors();
         *   check_rules();
         *   control_relays();
         */

        /* Brief sleep to avoid 100% CPU spin */
        ClockP_usleep(1000);  /* 1 ms */
    }

    /* Linux requested graceful shutdown - clean up and halt */
    doShutdown();
    /* doShutdown() never returns */
}
