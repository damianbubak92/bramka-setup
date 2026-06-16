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
#include <drivers/soc.h>

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
/* Set when Linux requests graceful shutdown via remoteproc */
static volatile uint8_t gbShutdown = 0u;
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

/* Send an EVENT (M4F-initiated message requiring ACK).
 * Adds to pending tracker for retry.
 * Returns 0 OK, -1 no slot, -2 encode/send fail. */
static int sendEvent(const uint8_t *payload, uint16_t payload_len)
{
    if (gLinuxEndpoint == 0) {
        return -1;  /* No Linux endpoint known yet */
    }
    if (payload_len > 128) {
        DebugP_log("[M4F] EVENT payload too large (%u, max 128)\r\n",
                    (unsigned)payload_len);
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
    pa->msg_type = MSG_EVENT;
    pa->retry_count = 0;
    pa->sent_at_us = ClockP_getTimeUsec();
    pa->payload_len = payload_len;
    for (uint16_t i = 0; i < payload_len; i++) pa->payload[i] = payload[i];

    /* Send */
    int32_t status = sendProtocolMsg(MSG_EVENT, pa->seq, payload, payload_len);
    if (status != SystemP_SUCCESS) {
        DebugP_log("[M4F] EVENT send failed: %d\r\n", status);
        pa->in_use = 0;
        return -2;
    }

    DebugP_log("[M4F] TX EVENT seq=%u (%u bytes)\r\n",
                (unsigned)pa->seq, (unsigned)payload_len);
    return 0;
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

    DebugP_log("[M4F] RX type=0x%02X seq=%u payload_len=%u\r\n",
                (unsigned)msg_type, (unsigned)msg_seq, (unsigned)msg_payload_len);

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
            DebugP_log("[M4F] RX heartbeat PING seq=%u - replying ACK\r\n",
                        (unsigned)msg_seq);
            sendAck(msg_seq);
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
    DebugP_log("[M4F] Closing drivers, deinit system, sending ACK\r\n");
    
    /* Match SDK example sequence exactly */
    Drivers_close();
    System_deinit();
    
    if (gbShutdownRemotecoreID != 0) {
        IpcNotify_sendMsg(gbShutdownRemotecoreID,
                          IPC_NOTIFY_CLIENT_ID_RP_MBOX,
                          IPC_NOTIFY_RP_MBOX_SHUTDOWN_ACK,
                          1u);
    }
    
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
    
    //testProtocol();

    gLastTickTime = ClockP_getTimeUsec();
    
/* Main loop - cooperative scheduler */
    while (!gbShutdown) {
        /* 1. Sprawdź czy callback dał nam wiadomość */
        if (gRxBuffer.pending) {
            handleLinuxMessage();
        }
        
        /* 2. Periodyczne akcje */
        doPeriodicTick();
        
        /* 3. Tu możesz dodać: sample_sensors(), check_rules(), control_relays() */
        processEventRetries();

        /* 4. Drobny sleep żeby CPU nie kręcił w pustce */
        ClockP_usleep(1000);  /* 1ms */
    }
    
    /* Linux requested shutdown - graceful cleanup */
    doShutdown();
    /* doShutdown() never returns - CPU halts in WFI */
}