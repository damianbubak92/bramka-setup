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

/* Linux chrdev expects endpoint 14 */
#define LINUX_CHRDEV_ENDPOINT       (14U)
#define LINUX_CHRDEV_SERVICE        "rpmsg_chrdev"
#define MAX_MSG_SIZE                (256u)
#define LINUX_CORE_ID               CSL_CORE_ID_A53SS0_0

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


static void handleLinuxMessage(void)
{
    uint8_t msg_type;
    uint16_t msg_seq;
    const uint8_t *msg_payload;
    uint16_t msg_payload_len;
    
    /* Dekoduj wiadomość jako binary protocol */
    int rc = protocol_decode((const uint8_t *)gRxBuffer.data, gRxBuffer.dataLen,
                              &msg_type, &msg_seq,
                              &msg_payload, &msg_payload_len);
    
    if (rc != 0) {
        DebugP_log("[M4F] Protocol decode failed: rc=%d (raw %u bytes)\r\n",
                    rc, (unsigned)gRxBuffer.dataLen);
        gRxBuffer.pending = 0;
        return;
    }
    
    /* Wykryj zmianę endpoint Linuxa */
    static uint16_t lastLoggedEndpoint = 0;
    if (gLinuxEndpoint != lastLoggedEndpoint) {
        DebugP_log("[M4F] Linux endpoint: %d (was %d)\r\n",
                    (int)gLinuxEndpoint, (int)lastLoggedEndpoint);
        lastLoggedEndpoint = gLinuxEndpoint;
    }
    
    DebugP_log("[M4F] RX type=0x%02X seq=%u payload_len=%u\r\n",
                (unsigned)msg_type, (unsigned)msg_seq, (unsigned)msg_payload_len);
    
    /* Obsługa per typ wiadomości */
    if (msg_type == MSG_HELLO) {
        /* Loguj payload jako string (dla debug) */
        char payload_str[64];
        size_t copy_len = (msg_payload_len < sizeof(payload_str)-1) 
                          ? msg_payload_len : sizeof(payload_str)-1;
        for (size_t i = 0; i < copy_len; i++) payload_str[i] = (char)msg_payload[i];
        payload_str[copy_len] = '\0';
        DebugP_log("[M4F] HELLO from Linux: '%s'\r\n", payload_str);
        
        /* Odpowiedz HELLO_ACK */
        uint8_t reply_buf[MSG_MAX_TOTAL];
        const char *reply_payload = "M4F v1 ready";
        size_t reply_payload_len = 12;  /* len("M4F v1 ready") */
        
        size_t reply_total_len = protocol_encode(
            reply_buf,
            MSG_HELLO_ACK,
            msg_seq,  /* odpowiadamy z tym samym seq */
            (const uint8_t *)reply_payload,
            reply_payload_len
        );
        
        int32_t status = RPMessage_send(
            reply_buf, reply_total_len,
            gRxBuffer.remoteCoreId, gRxBuffer.remoteEndPt,
            RPMessage_getLocalEndPt(&gRecvMsgObject),
            100
        );
        
        if (status == SystemP_SUCCESS) {
            DebugP_log("[M4F] HELLO_ACK sent (%u bytes)\r\n", 
                        (unsigned)reply_total_len);
        } else {
            DebugP_log("[M4F] HELLO_ACK send failed: %d\r\n", status);
        }
    }
    else {
        DebugP_log("[M4F] Unknown message type 0x%02X (ignored)\r\n", 
                    (unsigned)msg_type);
    }
    
    gRxBuffer.pending = 0;
}

/*
 * Periodyczna akcja - co 1 sekundę zwiększ licznik (i wyślij do Linuxa).
 * Wywoływane z main loop, sprawdza czy minęło 1000ms od ostatniego tick.
 */
static void doPeriodicTick(void)
{
    uint64_t now = ClockP_getTimeUsec();
    
    if ((now - gLastTickTime) >= 1000000ULL) {  /* 1 sekunda */
        gTickCount++;
        gLastTickTime = now;
        
      //  DebugP_log("[M4F] Tick #%u\r\n", (unsigned int)gTickCount);
        
        /* Server-push: wysyłaj tylko gdy znamy endpoint Linuxa */
     /*   if (gLinuxEndpoint != 0) {
    char tickMsg[64];
    int len = snprintf(tickMsg, sizeof(tickMsg),
                        "tick %u from M4F", (unsigned int)gTickCount);
    int32_t sendStatus = RPMessage_send(tickMsg, len + 1,
                                          LINUX_CORE_ID, gLinuxEndpoint,
                                          RPMessage_getLocalEndPt(&gRecvMsgObject),
                                          100);
    if (sendStatus != SystemP_SUCCESS) {
        DebugP_log("[M4F] Linux disconnected (send err %d), waiting for reconnect\r\n",
                    sendStatus);
        gLinuxEndpoint = 0;
    }
}
*/
/* else: gLinuxEndpoint == 0 - cicho czekamy na hello od Linuxa */
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
        
        /* 4. Drobny sleep żeby CPU nie kręcił w pustce */
        ClockP_usleep(1000);  /* 1ms */
    }
    
    /* Linux requested shutdown - graceful cleanup */
    doShutdown();
    /* doShutdown() never returns - CPU halts in WFI */
}