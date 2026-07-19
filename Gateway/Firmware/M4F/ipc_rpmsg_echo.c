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
#include "protocol.h"
#include "engine.h"
#include "engine_rpmsg.h"
#include "spi_master.h"
#include <drivers/soc.h>
#include <drivers/uart.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* The debug-log UART is configured in CALLBACK read mode by SysConfig
 * (example.syscfg: debug_log.uartLog.readCallbackFxn = "uart_echo_read_callback"),
 * so ti_drivers_open_close.c references this symbol. We don't use UART input -
 * provide a no-op so the link resolves. */
void uart_echo_read_callback(UART_Handle handle, UART_Transaction *trans)
{
    (void)handle;
    (void)trans;
}

/* Linux chrdev expects endpoint 14 */
#define LINUX_CHRDEV_ENDPOINT       (14U)
#define LINUX_CHRDEV_SERVICE        "rpmsg_chrdev"
#define MAX_MSG_SIZE                (256u)
#define LINUX_CORE_ID               CSL_CORE_ID_A53SS0_0

/* ========================================================================== */
/*  CUSTOM HARD FAULT HANDLER                                                 */
/* ========================================================================== */
/*
 * Domyślny SDK HwiP_hardFault_handler tylko spin'uje w while(loop) - Linux
 * nie ma jak wykryć że M4F died. Override przez modyfikację vector table.
 *
 * Strategia:
 *   1. Disable interrupts
 *   2. Wyślij IPC_NOTIFY_RP_MBOX_CRASH do Linux przez mailbox (SDK API)
 *      - Linux remoteproc framework rozpozna ten message i triggerze recovery
 *   3. Brief delay - daj mailbox czas na dostarczenie
 *   4. SCB AIRCR SYSRESETREQ jako backup - resetuje M4F core (jeśli mailbox
 *      nie zadziałał, Linux wykryje przez RPMsg silence)
 */

extern uint32_t gHwiP_vectorTable[];

/* ARM Cortex-M4 System Control Block AIRCR register */
#define SCB_AIRCR_ADDR        (0xE000ED0CUL)
#define SCB_AIRCR_VECTKEY     (0x05FAUL << 16)
#define SCB_AIRCR_SYSRESETREQ (1UL << 2)

void myHardFault_Handler(void) __attribute__((interrupt));
void myHardFault_Handler(void)
{
    /* M4F hard fault → triggerue full SoC reset przez TI SDK API.
     * Per TI forum: AM62 nie supportuje per-core reset, więc i tak resetujemy cały SoC.
     * To clean recovery - wszystko dostaje fresh boot. */
    
    /* Disable interrupts */
    __asm volatile ("cpsid i");
    
    /* TI-blessed SoC reset (na AM62 propaguje do obu domen) */
    SOC_generateSwWarmResetMcuDomain();
    
    /* Should never reach here */
    while (1) { }
}

static void installCustomHardFaultHandler(void)
{
    /* Override SDK default HardFault_Handler which only spins in while loop.
     * Our handler triggers full SoC reset via TI SDK API. */
    gHwiP_vectorTable[3] = (uint32_t)&myHardFault_Handler;
    
    __asm volatile ("dsb" ::: "memory");
    __asm volatile ("isb" ::: "memory");

    DebugP_log("[M4F] Custom HardFault_Handler installed (SOC_generateSwWarmResetMcuDomain)\r\n");
}

/* RPMessage object - musi być global */
static RPMessage_Object gRecvMsgObject;

/* Bufor dla wiadomości otrzymanej od Linuxa - wypełniany w callback */
static volatile struct {
    char     data[MAX_MSG_SIZE];
    uint16_t dataLen;
    uint16_t remoteCoreId;
    uint16_t remoteEndPt;
    volatile uint8_t  pending;  /* 1 = nowa wiadomość, 0 = pusty */
} gRxBuffer;

/* Bufor dla tick counter */
static uint32_t gTickCount = 0;
static uint64_t gLastTickTime = 0;

/* Endpoint Linuxa - 0 = jeszcze nie znamy.
 * Gdy Linux wyśle pierwszą wiadomość, callback zapisze tu jego endpoint.
 * Tick'i wysyłamy tylko do TEGO endpoint, nie do hardcoded.
 */
static volatile uint16_t gLinuxEndpoint = 0;

/* === PROTOCOL STATE === */

/* Last sequence number we received from Linux - for idempotency check.
 * 0 = nothing received yet. */
static volatile uint16_t gTheirLastSeq = 0;

/* Counter for our own outgoing seq numbers (for EVENT messages later) */
static volatile uint16_t gMySeq = 0;

/* === OUTGOING ACK TRACKING (for M4F-initiated messages: EVENT, DATA) ===
 * Statyczna tablica oczekujących ACK od Linuxa.
 * Bez malloc - typowe embedded.
 */
typedef struct {
    uint8_t  in_use;            /* 0=wolny, 1=oczekuje na ACK */
    uint16_t seq;               /* seq tej wiadomości */
    uint8_t  msg_type;          /* MSG_EVENT lub MSG_DATA */
    uint8_t  retry_count;       /* ile razy już retransmisja */
    uint64_t sent_at_us;        /* timestamp ostatniej wysyłki */
    uint16_t payload_len;       /* długość payload */
    uint8_t  payload[128];      /* sam payload (max 128B per entry) */
} pending_ack_t;

static pending_ack_t gPendingAcks[MAX_PENDING_ACKS];

/* === HEARTBEAT ===
 * M4F does NOT initiate heartbeat. The asymmetry is intentional: only Linux
 * pings M4F (Linux can reset M4F via remoteproc; M4F cannot reset Linux until
 * Warstwa C / DMSC is implemented). M4F only replies ACK to incoming PING
 * (see case MSG_PING). */


static pending_ack_t* findFreePendingSlot(void)
{
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (!gPendingAcks[i].in_use) return &gPendingAcks[i];
    }
    return NULL;
}

static pending_ack_t* findPendingBySeq(uint16_t seq)
{
    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        if (gPendingAcks[i].in_use && gPendingAcks[i].seq == seq) {
            return &gPendingAcks[i];
        }
    }
    return NULL;
}

/* === SHUTDOWN HANDLING === */
/* Set when Linux requests graceful shutdown via remoteproc.
 * Non-static: the SPI task (spi_master.c) also watches it to stop before
 * doShutdown() closes the drivers (MCSPI). */
volatile uint8_t gbShutdown = 0u;
static volatile uint8_t gbShutdownRemotecoreID = 0u;


/*
 * Callback wywoływany w kontekście wątku RPMsg gdy Linux wyśle wiadomość.
 * MUSI BYĆ KRÓTKI - tylko skopiuj dane i ustaw flagę.
 */
static void linuxMsgCallback(RPMessage_Object *obj, void *arg,
                              void *data, uint16_t dataLen,
                              uint16_t remoteCoreId, uint16_t remoteEndPt)
{
    /* Ignoruj jeśli poprzednia wiadomość nieobsłużona (lub zaimplementuj queue) */
    if (gRxBuffer.pending) {
        DebugP_log("[M4F] WARNING: dropped message, previous still pending\r\n");
        return;
    }
    
    if (dataLen > MAX_MSG_SIZE - 1) {
        dataLen = MAX_MSG_SIZE - 1;
    }
    
    memcpy((void *)gRxBuffer.data, data, dataLen);
    gRxBuffer.data[dataLen] = 0;  /* null terminator */
    gRxBuffer.dataLen = dataLen;
    gRxBuffer.remoteCoreId = remoteCoreId;
    gRxBuffer.remoteEndPt = remoteEndPt;

    /* Atomic ustawienie flagi - signal dla main loop */
    gRxBuffer.pending = 1;

    /* Aktualizuj endpoint Linuxa przy każdej wiadomości - obsługuje restart Linux service */
    gLinuxEndpoint = remoteEndPt;
}


/*
 * Callback dla shutdown notifications od Linux remoteproc driver.
 * Wywoływany w kontekście IPC notify - musi być KRÓTKI.
 * Sygnał dla main loop żeby graceful zakończyć.
 */
static void ipc_rp_mbox_callback(uint16_t remoteCoreId, uint16_t clientId,
                                  uint32_t msgValue, void *args)
{
    if (clientId == IPC_NOTIFY_CLIENT_ID_RP_MBOX) {
        if (msgValue == IPC_NOTIFY_RP_MBOX_SHUTDOWN) {
            /* Linux requests graceful shutdown */
            gbShutdown = 1u;
            gbShutdownRemotecoreID = (uint8_t)remoteCoreId;
            /* NIE używamy RPMessage_unblock bo używamy callback recv (nie blocking) */
        }
    }
}

/* Send a protocol message to Linux.
 * Returns SystemP_SUCCESS or error code from RPMessage_send. */
static int32_t sendProtocolMsg(uint8_t msg_type, uint16_t seq,
                                 const uint8_t *payload, uint16_t payload_len)
{
    if (gLinuxEndpoint == 0) {
        DebugP_log("[M4F] Cannot send 0x%02X seq=%u: no Linux endpoint\r\n",
                    (unsigned)msg_type, (unsigned)seq);
        return SystemP_FAILURE;
    }

    uint8_t enc_buf[MSG_MAX_TOTAL];
    size_t enc_len = protocol_encode(enc_buf, msg_type, seq, payload, payload_len);
    if (enc_len == 0) {
        DebugP_log("[M4F] Encode failed for 0x%02X (payload too large?)\r\n",
                    (unsigned)msg_type);
        return SystemP_FAILURE;
    }

    return RPMessage_send(enc_buf, enc_len,
                           LINUX_CORE_ID, gLinuxEndpoint,
                           RPMessage_getLocalEndPt(&gRecvMsgObject),
                           100);
}

/* Send ACK for a specific seq number */
static void sendAck(uint16_t seq)
{
    int32_t status = sendProtocolMsg(MSG_ACK, seq, NULL, 0);
    if (status != SystemP_SUCCESS) {
        DebugP_log("[M4F] sendAck seq=%u failed: %d\r\n", (unsigned)seq, status);
    }
}

/* Send a bare reply (no payload) echoing a seq - MSG_ACK or MSG_ERROR.
 * Fire-and-forget (no retry); used by the engine glue to confirm/reject
 * rule-push / node-cmd messages so Linux correlates by seq. */
static void sendReply(uint8_t type, uint16_t seq)
{
    int32_t status = sendProtocolMsg(type, seq, NULL, 0);
    if (status != SystemP_SUCCESS) {
        DebugP_log("[M4F] sendReply 0x%02X seq=%u failed: %d\r\n",
                    (unsigned)type, (unsigned)seq, status);
    }
}

/* Send a reliable M4F-initiated message (requiring ACK) of the given type.
 * Adds to pending tracker for retry. Used for EVENT and the gen2 reporters
 * (NODE_TELEMETRY/STATE/RULE_FIRED).
 * Returns 0 OK, -1 no slot/no endpoint, -2 encode/send fail. */
static int sendReliable(uint8_t msg_type, const uint8_t *payload, uint16_t payload_len)
{
    if (gLinuxEndpoint == 0) {
        return -1;  /* No Linux endpoint known yet */
    }
    if (payload_len > 128) {
        DebugP_log("[M4F] reliable 0x%02X payload too large (%u, max 128)\r\n",
                    (unsigned)msg_type, (unsigned)payload_len);
        return -2;
    }

    pending_ack_t *pa = findFreePendingSlot();
    if (pa == NULL) {
        DebugP_log("[M4F] No free pending slot (table full, %d entries)\r\n",
                    MAX_PENDING_ACKS);
        return -1;
    }

    /* Generate sequence number (1..65535, skip 0) */
    gMySeq++;
    if (gMySeq == 0) gMySeq = 1;

    /* Fill pending entry */
    pa->in_use = 1;
    pa->seq = gMySeq;
    pa->msg_type = msg_type;
    pa->retry_count = 0;
    pa->sent_at_us = ClockP_getTimeUsec();
    pa->payload_len = payload_len;
    for (uint16_t i = 0; i < payload_len; i++) pa->payload[i] = payload[i];

    /* Send */
    int32_t status = sendProtocolMsg(msg_type, pa->seq, payload, payload_len);
    if (status != SystemP_SUCCESS) {
        DebugP_log("[M4F] reliable 0x%02X send failed: %d\r\n",
                    (unsigned)msg_type, status);
        pa->in_use = 0;
        return -2;
    }

    DebugP_log("[M4F] TX 0x%02X seq=%u (%u bytes)\r\n",
                (unsigned)msg_type, (unsigned)pa->seq, (unsigned)payload_len);
    return 0;
}

/* EVENT convenience wrapper (unchanged external behavior). */
static int sendEvent(const uint8_t *payload, uint16_t payload_len)
{
    return sendReliable(MSG_EVENT, payload, payload_len);
}

/* ========================================================================== */
/*  ENGINE GLUE (FreeRTOS multi-task, lock-free via queues)                    */
/* ========================================================================== */
/*
 * Two tasks, no shared-state locking:
 *   - ENGINE task: evaluates rules - data rules on node-data arrival, time rules
 *     on a wall-clock minute tick aligned to :00. Rule firings / telemetry are
 *     pushed to gOutboxQueue. NEVER calls RPMessage_send and NEVER touches
 *     gPendingAcks.
 *   - COMMS task (= ipc_rpmsg_echo_main body): SOLE owner of RPMessage_send +
 *     gPendingAcks. Drains RX, drains gOutboxQueue (-> report to Linux + node
 *     TX), runs retries. The queues are the only cross-task channel (the gen1
 *     coreTask pattern: queues instead of mutexes).
 *
 * The active ruleset is the third cross-task touch point, but it is lock-free by
 * construction: engine_rules_commit() swaps a pointer-index atomically and
 * engine_evaluate() reads it once into a local, so a concurrent push never
 * corrupts an in-flight evaluation (see engine.c).
 */

typedef enum {
    OUTBOX_RULE_FIRED = 0,   /* msg=node command, ruleIndex+action=audit */
    OUTBOX_TELEMETRY  = 1,    /* msg=raw node reading -> Linux/cloud */
} outbox_kind_t;

typedef struct {
    outbox_kind_t kind;
    NodeFrame     msg;       /* factory_id + MessageStruct (telemetry: sender; cmd: target) */
    uint16_t      ruleIndex;
    RuleAction    action;
} outbox_item_t;

/* --- FreeRTOS objects (static allocation; no heap) --- */
#define ENGINE_TASK_STACK_WORDS   1024u   /* StackType_t units. [DIAG] confirmed ~300 words used (720 free at 1024), so no bump needed - and M4F_DRAM is nearly full. Headroom tracked via the [DIAG] stack-HWM self-scan. */
#define ENGINE_TASK_PRIORITY      2u      /* keep < comms task so RX/heartbeat win */
#define OUTBOX_DEPTH              16u
#define NODEIN_DEPTH              8u

static StackType_t  gEngineStack[ENGINE_TASK_STACK_WORDS];
static StaticTask_t gEngineTaskObj;

/* Free stack words remaining (high-water-mark): count words still holding the 0xA5
 * fill from the deep (low-address) end. Cortex-M stacks grow down, so the untouched
 * headroom sits at index 0+. Self-scan because the SDK FreeRTOS config has
 * INCLUDE_uxTaskGetStackHighWaterMark=0. The stack MUST be 0xA5-filled before the
 * task starts (done at creation). */
static uint32_t stack_hwm_words(const StackType_t *base, uint32_t words)
{
    uint32_t freeWords = 0u;
    while (freeWords < words && base[freeWords] == (StackType_t)0xA5A5A5A5u) {
        freeWords++;
    }
    return freeWords;
}

/* ---- COMMS liveness monitor (silent-hang diagnosis) --------------------------
 * The COMMS task (freertos_main, highest priority) owns the heartbeat reply. When
 * it silently stops replying (Go times out ~9 s later and reboots), we lose all
 * insight because a full SoC reset wipes retained RAM. This monitor - a lower-prio
 * task that runs whenever COMMS yields (its per-loop vTaskDelay) or BLOCKS (the
 * likely hang: pending forever in RPMessage_send / an SPI wait) - watches a
 * liveness counter and a per-stage marker, and logs WHICH stage COMMS froze in.
 * The log lands in the m4f-watch trace within the ~9 s window before the reboot.
 * (A busy-loop at max prio would starve this monitor too - then the [DIAG]/[MON]
 * lines just stop, which itself says "total starve, not a block".) */
#define MONITOR_TASK_STACK_WORDS  512u
#define MONITOR_TASK_PRIORITY     3u    /* > engine/spi (2), < comms (max) */
#define COMMS_STALL_CHECKS        3u    /* x2 s = 6 s, under Go's ~9 s heartbeat giveup */

static StackType_t  gMonitorStack[MONITOR_TASK_STACK_WORDS];
static StaticTask_t gMonitorTaskObj;

volatile uint32_t gCommsLive  = 0u;   /* bumped once per COMMS loop iteration */
volatile uint8_t  gCommsStage = 0u;   /* 1=rx 2=drain 3=tick 4=retry 5=sleep */

static void monitorTask(void *args)
{
    uint32_t lastLive = 0u;
    uint32_t stalls   = 0u;
    (void)args;
    DebugP_log("[MON] comms-liveness monitor started (prio %u)\r\n",
               (unsigned)MONITOR_TASK_PRIORITY);
    while (!gbShutdown) {
        vTaskDelay(pdMS_TO_TICKS(2000u));
        if (gCommsLive == lastLive) {
            if (++stalls >= COMMS_STALL_CHECKS) {
                DebugP_log("[MON] *** COMMS STALLED at stage=%u (1rx 2drain 3tick 4retry 5sleep), "
                           "live=%u frozen ~%us - Linux heartbeat will reboot us ***\r\n",
                           (unsigned)gCommsStage, (unsigned)gCommsLive, (unsigned)(stalls * 2u));
            }
        } else {
            stalls = 0u;
        }
        lastLive = gCommsLive;
    }
    vTaskDelete(NULL);
}

static uint8_t      gOutboxStorage[OUTBOX_DEPTH * sizeof(outbox_item_t)];
static StaticQueue_t gOutboxQueueObj;
static QueueHandle_t gOutboxQueue;

/* Node data INTO the engine (SPI -> here). Created now for the final task shape;
 * fed once the CC1310 SPI link exists. */
static uint8_t      gNodeInStorage[NODEIN_DEPTH * sizeof(NodeFrame)];
static StaticQueue_t gNodeInQueueObj;
static QueueHandle_t gNodeInQueue;

/* Internal sentinel posted on gNodeInQueue (COMMS task) to wake the ENGINE task
 * after a time-sync: it runs EVAL_TIME promptly and re-aligns the tick to :00
 * immediately, instead of waiting out its in-progress sleep. Value is outside
 * the NODE_* range (0..5) and never appears on the wire. */
#define ENGINE_NODEIN_TIME_RESYNC   0xEEu

/* Monotonic microsecond clock for the engine wall-clock (injected at init so
 * engine.c stays SDK-agnostic). */
static uint64_t engineClockUsec(void)
{
    return ClockP_getTimeUsec();
}

/* Action sink (ENGINE task context): a rule fired. Push to the outbox; the comms
 * task does the actual Linux report + node delivery. No RPMessage here. */
static void engineActionSink(const MessageStruct *msg, uint16_t ruleIndex,
                             const RuleAction *action, void *ctx)
{
    (void)ctx;
    outbox_item_t item;
    item.kind = OUTBOX_RULE_FIRED;
    /* Engine-fired command: the engine works on addresses, not chip ids, so the
     * NodeFrame carries a zero factory_id -> the CC1310 sends a legacy 'D' frame
     * (its targets today are the gen1 legacy nodes). A future rule-controlled gen2
     * node would need the engine to cache addr->factory_id from uplinks. */
    memset(&item.msg, 0, sizeof(item.msg));
    item.msg.msg = *msg;
    item.ruleIndex = ruleIndex;
    item.action = *action;
    if (xQueueSend(gOutboxQueue, &item, 0) != pdTRUE) {
        DebugP_log("[M4F] outbox full - rule #%u action dropped\r\n",
                    (unsigned)ruleIndex);
    }
}

/* Node TX sink (COMMS task context): relay a command down to a node over SPI.
 * Hands off to the SPI master task (non-blocking); the SPI task owns the bus. */
static void nodeTxSink(const NodeFrame *frame)
{
    if (!spi_master_post_cmd(frame)) {
        DebugP_log("[M4F] -> node: SPI out queue full (type=%u cmd=%u)\r\n",
                    (unsigned)frame->msg.type, (unsigned)frame->msg.cmd);
    }
}

/* ENGINE task: data rules are event-driven (node-data queue); time rules run on
 * a wall-clock minute tick aligned to :00. The queue receive timeout is sized to
 * the next minute boundary, so an intervening node event does not shift the tick
 * (recomputed each loop) - keeps TIME evaluation deterministic at :00. */
static void engineTask(void *args)
{
    (void)args;
    NodeFrame nodeMsg;   /* factory_id + MessageStruct from the SPI link */

    DebugP_log("[M4F] Engine task started\r\n");
    while (!gbShutdown) {
        uint32_t ms = engine_ms_to_next_minute();  /* to next :00, sub-second precise */
        if (xQueueReceive(gNodeInQueue, &nodeMsg, pdMS_TO_TICKS(ms)) == pdTRUE) {
            if (nodeMsg.msg.type == ENGINE_NODEIN_TIME_RESYNC) {
                /* Time-sync sentinel: wake ONLY to break a stale (pre-sync) sleep
                 * so the next loop re-sizes the timeout from the fresh clock and
                 * aligns to :00. Do NOT evaluate here - evaluating off the :00
                 * boundary would fire early (and double up with the boundary
                 * timeout). No-op on purpose; fall through to recompute. */
            } else {
                /* Node data arrived. Forward the RAW reading to Linux (telemetry
                 * -> DB) for EVERY node type, independently of whether the engine
                 * recognises it: engine_update_node() only folds known types
                 * (solar/bufor) into NodesData and returns false for the rest, but
                 * the DB must still receive e.g. a T/H sensor. Then fold the known
                 * types and evaluate the data-driven rules (COND_PARAMETER/_DELTA). */
                outbox_item_t item;
                item.kind = OUTBOX_TELEMETRY;
                item.msg = nodeMsg;                    /* NodeFrame: factory_id + reading */
                if (xQueueSend(gOutboxQueue, &item, 0) != pdTRUE) {
                    DebugP_log("[M4F] outbox full - telemetry dropped\r\n");
                }
                (void)engine_update_node(&nodeMsg.msg);   /* fold known types into live state */
                engine_evaluate(ENGINE_EVAL_NODE);
            }
        } else {
            /* Timeout = we slept the full span to the :00 boundary: evaluate the
             * time-driven rules (COND_TIME). The sentinel guarantees this sleep
             * was sized by a valid clock (it breaks the pre-sync sleep), so the
             * timeout only ever lands on :00. */
            engine_evaluate(ENGINE_EVAL_TIME);
        }
    }
    vTaskDelete(NULL);
}

/* Drain the engine outbox (COMMS task context: owns RPMessage send). */
static void drainEngineOutbox(void)
{
    outbox_item_t out;
    while (xQueueReceive(gOutboxQueue, &out, 0) == pdTRUE) {
        if (out.kind == OUTBOX_RULE_FIRED) {
            engine_rpmsg_report_rule_fired(out.ruleIndex, &out.action);
            nodeTxSink(&out.msg);
        } else {
            engine_rpmsg_report_telemetry(&out.msg);
        }
    }
}

/* Periodically check pending entries, retry timed-out, giveup after MAX_RETRIES */
static void processEventRetries(void)
{
    uint64_t now = ClockP_getTimeUsec();
    uint64_t timeout_us = (uint64_t)ACK_TIMEOUT_MS * 1000ULL;

    for (int i = 0; i < MAX_PENDING_ACKS; i++) {
        pending_ack_t *pa = &gPendingAcks[i];
        if (!pa->in_use) continue;

        uint64_t elapsed = now - pa->sent_at_us;
        if (elapsed < timeout_us) continue;

        if (pa->retry_count >= MAX_RETRIES) {
            DebugP_log("[M4F] GIVEUP seq=%u type=0x%02X (%u retries exhausted)\r\n",
                        (unsigned)pa->seq,
                        (unsigned)pa->msg_type,
                        (unsigned)pa->retry_count);

            pa->in_use = 0;
            continue;
        }

        pa->retry_count++;
        pa->sent_at_us = now;
        DebugP_log("[M4F] RETRY seq=%u type=0x%02X count=%u\r\n",
            (unsigned)pa->seq, (unsigned)pa->msg_type, (unsigned)pa->retry_count);

        sendProtocolMsg(pa->msg_type, pa->seq, pa->payload, pa->payload_len);
    }
}

static void handleLinuxMessage(void)
{
    uint8_t msg_type;
    uint16_t msg_seq;
    const uint8_t *msg_payload;
    uint16_t msg_payload_len;

    /* Decode binary protocol message */
    int rc = protocol_decode(
        (const uint8_t *)gRxBuffer.data,
        gRxBuffer.dataLen,
        &msg_type, &msg_seq,
        &msg_payload, &msg_payload_len);

    if (rc != 0) {
        DebugP_log("[M4F] Protocol decode failed: rc=%d (raw %u bytes)\r\n",
                    rc, (unsigned)gRxBuffer.dataLen);
        gRxBuffer.pending = 0;
        return;
    }

    /* Track Linux endpoint - log changes */
    static uint16_t lastLoggedEndpoint = 0;
    if (gLinuxEndpoint != lastLoggedEndpoint) {
        DebugP_log("[M4F] Linux endpoint: %d (was %d)\r\n",
                    (int)gLinuxEndpoint, (int)lastLoggedEndpoint);
        lastLoggedEndpoint = gLinuxEndpoint;
    }

    /* Heartbeat PING is silenced to keep the m4f-watch trace readable (it ticks
     * every few seconds and works). Uncomment the guard to log it again. */
    if (msg_type != MSG_PING) {
        DebugP_log("[M4F] RX type=0x%02X seq=%u payload_len=%u\r\n",
                    (unsigned)msg_type, (unsigned)msg_seq, (unsigned)msg_payload_len);
    }

    /* Dispatch by message type */
    switch (msg_type) {
        case MSG_HELLO: {
            /* Log payload as string */
            char payload_str[64];
            size_t copy_len = (msg_payload_len < sizeof(payload_str)-1) 
                              ? msg_payload_len : sizeof(payload_str)-1;
            for (size_t i = 0; i < copy_len; i++) payload_str[i] = (char)msg_payload[i];
            payload_str[copy_len] = '\0';
            DebugP_log("[M4F] HELLO from Linux: '%s'\r\n", payload_str);

            /* Reset protocol state on new HELLO (handles Linux restart) */
            gTheirLastSeq = 0;
            gMySeq = 0;
            /* Note: don't clear gPendingAcks here - retries will time out naturally */
            DebugP_log("[M4F] Protocol state RESET (new session)\r\n");

            /* Reply HELLO_ACK with the same seq */
            const uint8_t reply_payload[] = "M4F v1 ready";
            int32_t status = sendProtocolMsg(MSG_HELLO_ACK, msg_seq,
                                               reply_payload, 12);
            if (status == SystemP_SUCCESS) {
                DebugP_log("[M4F] HELLO_ACK sent (seq=%u)\r\n", (unsigned)msg_seq);
            } else {
                DebugP_log("[M4F] HELLO_ACK send failed: %d\r\n", status);
            }
            break;
        }

        case MSG_DATA: {
            /* Idempotency check */
            if (gTheirLastSeq != 0 && msg_seq <= gTheirLastSeq) {
                DebugP_log("[M4F] DATA seq=%u DUPLICATE (lastSeq=%u) - re-ACK only\r\n",
                            (unsigned)msg_seq, (unsigned)gTheirLastSeq);
                sendAck(msg_seq);
                break;
            }

            /* New message - update lastSeq, log payload */
            gTheirLastSeq = msg_seq;

            char payload_str[80];
            size_t copy_len = (msg_payload_len < sizeof(payload_str)-1) 
                              ? msg_payload_len : sizeof(payload_str)-1;
            for (size_t i = 0; i < copy_len; i++) payload_str[i] = (char)msg_payload[i];
            payload_str[copy_len] = '\0';
            DebugP_log("[M4F] DATA seq=%u: '%s'\r\n",
                        (unsigned)msg_seq, payload_str);

            /* Send ACK */
            sendAck(msg_seq);
            DebugP_log("[M4F] ACK sent for seq=%u\r\n", (unsigned)msg_seq);

            /* TODO: dispatch to application logic */
            break;
        }

        case MSG_ACK: {
            pending_ack_t *pa = findPendingBySeq(msg_seq);
            if (pa != NULL) {
                uint64_t rtt_us = ClockP_getTimeUsec() - pa->sent_at_us;
                uint32_t rtt_ms = (uint32_t)(rtt_us / 1000);
                DebugP_log("[M4F] ACK seq=%u type=0x%02X (RTT %u ms, retries=%u)\r\n",
                            (unsigned)msg_seq,
                            (unsigned)pa->msg_type,
                            (unsigned)rtt_ms,
                            (unsigned)pa->retry_count);

                pa->in_use = 0;
            } else {
                DebugP_log("[M4F] ACK for unknown seq=%u (already acked or never sent?)\r\n",
                            (unsigned)msg_seq);
            }
            break;
        }

        case MSG_PING: {
            /* Heartbeat from Linux - reply with generic ACK.
             * MSG_PONG is deprecated, all confirmations go through MSG_ACK now.
             * M4F never initiates PING itself (one-way heartbeat: Linux->M4F). */
            /* Silenced (heartbeat works; uncomment to debug):
             * DebugP_log("[M4F] RX heartbeat PING seq=%u - replying ACK\r\n",
             *             (unsigned)msg_seq); */
            sendAck(msg_seq);
            break;
        }

        case MSG_RULE_BEGIN:
        case MSG_RULE_ITEM:
        case MSG_RULE_COMMIT:
        case MSG_NODE_CMD:
        case MSG_TIME_SYNC: {
            /* gen2 control messages -> engine glue (handles ACK/ERROR). */
            engine_rpmsg_dispatch(msg_type, msg_seq, msg_payload, msg_payload_len);

            /* Wake the engine task after a clock update so it re-aligns its
             * minute tick to :00 immediately (engine_set_time already applied
             * inside the dispatch). Sentinel, best-effort. */
            if (msg_type == MSG_TIME_SYNC) {
                NodeFrame wake;
                memset(&wake, 0, sizeof(wake));
                wake.msg.type = ENGINE_NODEIN_TIME_RESYNC;
                (void)xQueueSend(gNodeInQueue, &wake, 0);
            }
            break;
        }

        case MSG_DEBUG_CRASH: {
            DebugP_log("[M4F] *** DEBUG: CRASH TRIGGER RECEIVED ***\r\n");
            DebugP_log("[M4F] Forcing hard fault via undefined instruction in 100ms\r\n");
            DebugP_log("[M4F] Expect: HardFault_Handler -> SOC reset -> bramka reboot\r\n");
            
            ClockP_usleep(100000);
            
            /* Undefined Thumb-2 instruction - guaranteed hard fault on Cortex-M */
            __asm volatile (".word 0xDEADDEAD");
            
            /* Should never reach here */
            while (1) { ; }
            break;
        }

        case MSG_DEBUG_HANG: {
                DebugP_log("[M4F] *** DEBUG: SILENT HANG TRIGGER RECEIVED ***\r\n");
                DebugP_log("[M4F] Entering infinite loop - all M4F processing stops\r\n");
                DebugP_log("[M4F] Linux heartbeat PINGs should giveup after ~5+3s\r\n");
                DebugP_log("[M4F] Recovery: Linux must force m4f-reload\r\n");

                /* Disable interrupts - even Linux remoteproc stop won't help.
                * Only full SoC reset or external power cycle recovers. */
                __asm volatile ("cpsid i");

                /* Completely freeze - no ClockP_usleep, no anything */
                while (1) {
                    /* nop */
                }
                break;  /* never reached */
        }

        default:
            DebugP_log("[M4F] Unknown msg type 0x%02X seq=%u (ignored)\r\n",
                        (unsigned)msg_type, (unsigned)msg_seq);
            break;
    }

    gRxBuffer.pending = 0;
}

static void doPeriodicTick(void)
{
    uint64_t now = ClockP_getTimeUsec();

    if ((now - gLastTickTime) >= 1000000ULL) {
        gTickCount++;
        gLastTickTime = now;

        /* Engine evaluation runs in the ENGINE task (node events + :00-aligned
         * minute tick), not here. doPeriodicTick stays for comms-side periodic
         * needs only. */

        /* Production: no autonomous tick log, no autonomous test EVENT.
         *
         * The per-second "Tick #N" log spammed the m4f-watch trace and hurt
         * readability, so it's disabled. The 10s test EVENT was scaffolding:
         * it fired whenever gLinuxEndpoint != 0 (which stays set forever after
         * first contact), so after the Linux service stopped/restarted the
         * EVENTs got no ACK -> retries -> GIVEUP / "ACK for unknown" noise.
         *
         * Real M4F->Linux EVENTs must be emitted by actual event sources
         * (sensor reading, alert, GPIO edge) via sendEvent() when they occur,
         * not on a free-running timer. sendEvent() itself is unchanged.
         *
         * To re-enable the demo EVENT for testing, uncomment below:
         *
         * DebugP_log("[M4F] Tick #%u\r\n", (unsigned int)gTickCount);
         * if (gLinuxEndpoint != 0 && (gTickCount % 10) == 0) {
         *     char event_payload[64];
         *     int len = snprintf(event_payload, sizeof(event_payload),
         *                         "Test EVENT @ tick %u", (unsigned int)gTickCount);
         *     if (len > 0 && len < (int)sizeof(event_payload)) {
         *         int rc = sendEvent((const uint8_t *)event_payload, (uint16_t)len);
         *         if (rc != 0) {
         *             DebugP_log("[M4F] sendEvent rc=%d\r\n", rc);
         *         }
         *     }
         * }
         */
        /* Stack-headroom watch (every 30 s). HWM = FREE words still holding the
         * 0xA5 fill at the deep (low-address) end of the task stack; trending toward
         * 0 is the prime suspect for the intermittent, traffic-correlated hang (an
         * overflow corrupts memory and wedges the core). Self-scan is used because
         * the SDK FreeRTOS config has INCLUDE_uxTaskGetStackHighWaterMark=0. */
        if ((gTickCount % 30U) == 0U) {
            /* stacks: free words (of 1024). spi xfer: started/done should track each
             * other; if `to` (timeout) climbs and `done` stalls, a stuck SPI transfer
             * is wedging the M4F (user hint - the whole core hangs on an unclosed SPI). */
            uint32_t spiStarted = 0u, spiDone = 0u, spiTo = 0u;
            spi_master_counters(&spiStarted, &spiDone, &spiTo);
            DebugP_log("[DIAG] stack free: engine=%u spi=%u | spi xfer started=%u done=%u to=%u\r\n",
                       (unsigned)stack_hwm_words(gEngineStack, ENGINE_TASK_STACK_WORDS),
                       (unsigned)spi_master_stack_hwm(),
                       (unsigned)spiStarted, (unsigned)spiDone, (unsigned)spiTo);
        }

        (void)gTickCount; /* still counted; used only by the commented demo above */
    }
}


/*
 * Graceful shutdown sequence.
 * Wykonywany po wyjściu z main loop (gdy gbShutdown == 1).
 * Po tej funkcji M4F wchodzi w WFI i Linux dokończy stop.
 */
static void doShutdown(void)
{
    DebugP_log("[M4F] Shutdown requested by core %u\r\n",
                (unsigned int)gbShutdownRemotecoreID);

    /* Stop the SPI task + tear down the SLAVE_READY GPIO-IRQ (Hwi + bank intr +
     * Sciclient introuter route). Bounded/best-effort - won't block the stop. */
    spi_master_shutdown(1000u);
    DebugP_log("[M4F] SPI stopped; sending shutdown ACK\r\n");

    /* Send the ACK FIRST, then halt. Linux completes the remoteproc stop on this
     * ACK. We deliberately SKIP Drivers_close()/System_deinit(): on AM62 with the
     * GPIO introuter route they can block here, so the ACK never goes out and the
     * stop hangs ("M4F won't stop") - exactly what the trace showed. The core is
     * reset + re-init'd by Linux on the next load, so that SDK teardown is not
     * needed for a clean reload; getting the ACK out reliably is what matters. */
    if (gbShutdownRemotecoreID != 0) {
        IpcNotify_sendMsg(gbShutdownRemotecoreID,
                          IPC_NOTIFY_CLIENT_ID_RP_MBOX,
                          IPC_NOTIFY_RP_MBOX_SHUTDOWN_ACK,
                          1u);
    }
    DebugP_log("[M4F] ACK sent; halting (WFI)\r\n");

    /* Halt CPU */
    while (1) {
        __asm__ __volatile__ ("wfi" "\n\t": : : "memory");
    }
}

static void testProtocol(void)
{
    DebugP_log("\r\n=== M4F PROTOCOL TEST ===\r\n");
    DebugP_log("CRC16-CCITT test vectors:\r\n");
    
    /* CRC test vectors - identyczne z Go: */
    uint8_t empty[1] = {0};       /* len=0 wykorzystany */
    uint8_t single_zero[1] = {0x00};
    uint8_t single_ff[1] = {0xFF};
    uint8_t hello[] = "hello";    /* 5 chars, no null */
    uint8_t hex_seq[5] = {0x01, 0x02, 0x03, 0x04, 0x05};
    uint8_t zeros16[16] = {0};
    uint8_t ffs16[16];
    uint8_t msg_hdr[5] = {0x10, 0x00, 0x05, 0x00, 0x04};
    int i;
    
    for (i = 0; i < 16; i++) ffs16[i] = 0xFF;
    
    DebugP_log("  empty                len= 0  crc=0x%04X\r\n", protocol_crc16(empty, 0));
    DebugP_log("  single zero          len= 1  crc=0x%04X\r\n", protocol_crc16(single_zero, 1));
    DebugP_log("  single 0xFF          len= 1  crc=0x%04X\r\n", protocol_crc16(single_ff, 1));
    DebugP_log("  hello                len= 5  crc=0x%04X\r\n", protocol_crc16(hello, 5));
    DebugP_log("  hex sequence         len= 5  crc=0x%04X\r\n", protocol_crc16(hex_seq, 5));
    DebugP_log("  all zeros 16B        len=16  crc=0x%04X\r\n", protocol_crc16(zeros16, 16));
    DebugP_log("  all 0xFF 16B         len=16  crc=0x%04X\r\n", protocol_crc16(ffs16, 16));
    DebugP_log("  msg header sample    len= 5  crc=0x%04X\r\n", protocol_crc16(msg_hdr, 5));
    
    DebugP_log("\r\nRound-trip encode/decode:\r\n");
    
    /* Encode "hello world" jako DATA seq=42 - musi dać IDENTYCZNE bajty jak Go */
    uint8_t enc_buf[MSG_MAX_TOTAL];
    uint8_t payload[] = "hello world";  /* 11 chars, no null */
    
    size_t enc_len = protocol_encode(enc_buf, MSG_DATA, 42, payload, 11);
    
    DebugP_log("  encoded %u bytes:\r\n", (unsigned)enc_len);
    DebugP_log("  ");
    for (i = 0; i < (int)enc_len; i++) {
        DebugP_log("%02X ", enc_buf[i]);
    }
    DebugP_log("\r\n");
    
    /* Decode the just-encoded message - sanity check */
    uint8_t  dec_type;
    uint16_t dec_seq;
    const uint8_t *dec_payload;
    uint16_t dec_payload_len;
    
    int rc = protocol_decode(enc_buf, enc_len,
                              &dec_type, &dec_seq,
                              &dec_payload, &dec_payload_len);
    if (rc == 0) {
        DebugP_log("  decoded: type=0x%02X seq=%u payload_len=%u\r\n",
                    (unsigned)dec_type, (unsigned)dec_seq, (unsigned)dec_payload_len);
        DebugP_log("  payload: '");
        for (i = 0; i < dec_payload_len; i++) {
            DebugP_log("%c", (char)dec_payload[i]);
        }
        DebugP_log("'\r\n");
        DebugP_log("  ROUND-TRIP OK\r\n");
    } else {
        DebugP_log("  decode FAILED: rc=%d\r\n", rc);
    }
    
    DebugP_log("=== END PROTOCOL TEST ===\r\n\r\n");
}

/*
 * Główny entry point - wywoływany przez SDK noRTOS po inicjalizacji.
 */
void ipc_rpmsg_echo_main(void *args)
{
    int32_t status;
    RPMessage_CreateParams createParams;
    
    /* Install custom HardFault_Handler for Linux recovery */
    installCustomHardFaultHandler();

    DebugP_log("[M4F] Application starting...\r\n");
    
    /* Inicjalizacja bufora */
    gRxBuffer.pending = 0;
    
    /* Czekaj aż Linux się zinicjalizuje */
    DebugP_log("[M4F] Waiting for Linux ready...\r\n");
    status = RPMessage_waitForLinuxReady(SystemP_WAIT_FOREVER);
    DebugP_assert(status == SystemP_SUCCESS);
    DebugP_log("[M4F] Linux ready!\r\n");
    
    /* Utwórz endpoint z callback dla async odbioru */
    RPMessage_CreateParams_init(&createParams);
    createParams.localEndPt = LINUX_CHRDEV_ENDPOINT;
    createParams.recvCallback = linuxMsgCallback;
    createParams.recvCallbackArgs = NULL;
    
    status = RPMessage_construct(&gRecvMsgObject, &createParams);
    DebugP_assert(status == SystemP_SUCCESS);
    
    /* Ogłoś endpoint Linuxowi */
    status = RPMessage_announce(LINUX_CORE_ID, LINUX_CHRDEV_ENDPOINT,
                                  LINUX_CHRDEV_SERVICE);
    DebugP_assert(status == SystemP_SUCCESS);
    DebugP_log("[M4F] Endpoint '%s' announced on EP %d\r\n",
                LINUX_CHRDEV_SERVICE, LINUX_CHRDEV_ENDPOINT);
    
    /* Zarejestruj callback dla Linux remoteproc shutdown notifications */
    status = IpcNotify_registerClient(IPC_NOTIFY_CLIENT_ID_RP_MBOX,
                                       ipc_rp_mbox_callback,
                                       NULL);
    DebugP_assert(status == SystemP_SUCCESS);
    DebugP_log("[M4F] Shutdown handler registered\r\n");
    
    DebugP_log("[M4F] Ready for messages\r\n");

    /* Automation engine: init core + RPMsg glue. Active ruleset starts empty;
     * Linux pushes it via RULE_BEGIN..COMMIT after HELLO. */
    engine_init(engineActionSink, NULL, engineClockUsec);
    engine_rpmsg_init(sendReliable, sendReply, nodeTxSink);

    /* Cross-task queues (lock-free): engine->comms outbox, SPI->engine node-in. */
    gOutboxQueue = xQueueCreateStatic(OUTBOX_DEPTH, sizeof(outbox_item_t),
                                       gOutboxStorage, &gOutboxQueueObj);
    gNodeInQueue = xQueueCreateStatic(NODEIN_DEPTH, sizeof(NodeFrame),
                                       gNodeInStorage, &gNodeInQueueObj);
    DebugP_assert(gOutboxQueue != NULL && gNodeInQueue != NULL);

    /* Spawn the ENGINE task; this task continues as the COMMS task. */
    memset(gEngineStack, 0xA5, sizeof(gEngineStack));   /* fill for the stack-HWM diagnostic */
    (void)xTaskCreateStatic(engineTask, "engine", ENGINE_TASK_STACK_WORDS, NULL,
                             ENGINE_TASK_PRIORITY, gEngineStack, &gEngineTaskObj);

    /* COMMS-liveness monitor: logs which stage COMMS froze in on a silent hang. */
    (void)xTaskCreateStatic(monitorTask, "monitor", MONITOR_TASK_STACK_WORDS, NULL,
                             MONITOR_TASK_PRIORITY, gMonitorStack, &gMonitorTaskObj);
    DebugP_log("[M4F] Engine initialized (0 rules); engine task spawned\r\n");

    /* SPI master link to the CC1310 (slave): feeds node data into gNodeInQueue
     * and drains nodeTxSink commands to nodes. Drivers_open() already opened
     * MCSPI. (Requires MCU_SPI0 + the 2 handshake GPIO in syscfg.) */
    spi_master_init(gNodeInQueue);

    //testProtocol();

    gLastTickTime = ClockP_getTimeUsec();
    
/* COMMS task loop - sole owner of RPMessage send + gPendingAcks */
    while (!gbShutdown) {
        gCommsLive++;   /* liveness beat for the monitor task (silent-hang diagnosis) */

        /* 1. RX from Linux (callback set the pending flag) */
        gCommsStage = 1u;
        if (gRxBuffer.pending) {
            handleLinuxMessage();
        }

        /* 2. Engine outbox -> Linux reports + node delivery */
        gCommsStage = 2u;
        drainEngineOutbox();

        /* 3. Periodic comms-side actions */
        gCommsStage = 3u;
        doPeriodicTick();

        /* 4. Retry M4F-initiated reliable messages */
        gCommsStage = 4u;
        processEventRetries();

        gCommsStage = 5u;
        /* 5. Block 1 tick so the (lower-priority) ENGINE task gets to run.
         * MUST be vTaskDelay (not ClockP_usleep): this comms task runs at the
         * highest priority (freertos_main), so a busy-wait here would starve
         * the engine task entirely. */
        vTaskDelay(1U);
    }
    
    /* Linux requested shutdown - graceful cleanup */
    doShutdown();
    /* doShutdown() never returns - CPU halts in WFI */
}