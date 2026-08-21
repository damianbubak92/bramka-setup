/*
 *  ======== lightswitch_node ========
 *
 *  Główny wątek: touch (Sensor Controller) + RF gen2 (JOIN/provisioning/§14) + przekaźnik.
 *   - krótki dotyk pada          -> toggle przekaźnika światła (CONFIG_RELAY1 = DIO28)
 *   - przytrzymanie pada 4 s      -> tryb parowania: czerwona dioda (DIO25) miga + wyślij JOIN
 *   - komendy z bramki (RF)       -> app task (node_app.c): JOIN_ACCEPT/REMOVE/SET_RELAY
 *
 *  Touch = elektroda DIO27 (area 0), ref R14 1.2M = DIO24. LED_RED = DIO25 (active-high NPN).
 *  UART/Display = DIO8/DIO9 (pogo). UWAGA: po flashu CCS zrób RESET.
 */

#include <stdint.h>
#include <unistd.h>

#include <ti/drivers/GPIO.h>
#include <ti/display/Display.h>

#include "ti_drivers_config.h"
#include "scif.h"
#include "rf_node.h"        /* radioTaskInit, rf_send_join */
#include "node_app.h"       /* appTaskInit, app_relay_toggle */
#include "node_identity.h"  /* gNodeAddress, ADDR_UNPROVISIONED */

#ifndef BV
#define BV(n)   (1U << (n))
#endif

#define TOUCH_AREA          0
#define POLL_MS             20
#define HOLD_JOIN_MS        4000        /* 4 s przytrzymania -> JOIN */
#define TOUCH_MIN_MS        60          /* min. czas dotyku (po debounce) */
#define DEBOUNCE_MS         80          /* stan dotyku musi być stabilny tyle, zanim się zaliczy */
#define PAIRING_WINDOW_MS   60000       /* miganie RED do accept lub 60 s */
#define PAIRING_BLINK_MS    240         /* okres przełączania RED (wielokr. POLL_MS) */

/* Współdzielony uchwyt Display (rfEchoTx.c i node_app.c robią extern). */
Display_Handle display;

/* Stan dotyku aktualizowany w kontekście ISR (callback ALERT Sensor Controllera). */
static volatile uint16_t gTouchState = 0U;

static void scCtrlReadyCallback(void) { }

static void scTaskAlertCallback(void)
{
    scifClearAlertIntSource();
    while (scifGetTaskIoStructAvailCount(SCIF_CAPACITIVE_TOUCH_TASK_ID, SCIF_STRUCT_OUTPUT) > 0)
    {
        SCIF_CAPACITIVE_TOUCH_OUTPUT_T *pOut =
            (SCIF_CAPACITIVE_TOUCH_OUTPUT_T *)scifGetTaskStruct(
                SCIF_CAPACITIVE_TOUCH_TASK_ID, SCIF_STRUCT_OUTPUT);
        gTouchState = pOut->pTouchDet[TOUCH_AREA];
        scifHandoffTaskStruct(SCIF_CAPACITIVE_TOUCH_TASK_ID, SCIF_STRUCT_OUTPUT);
    }
    scifAckAlertEvents();
}

void *mainThread(void *arg0)
{
    (void)arg0;

    GPIO_init();
    display = Display_open(Display_Type_UART, NULL);

    /* LED_RED (pairing) - wyjście, zgaszona. */
    GPIO_setConfig(CONFIG_GPIO_RLED, GPIO_CFG_OUT_STD | GPIO_CFG_OUT_LOW);

    Display_printf(display, 0, 0, "\r\n=== lightswitch_node ===");

    /* Sensor Controller cap-sense (touch) */
    scifOsalInit();
    scifOsalRegisterCtrlReadyCallback(scCtrlReadyCallback);
    scifOsalRegisterTaskAlertCallback(scTaskAlertCallback);
    scifInit(&scifDriverSetup);
    scifWaitOnNbl(1000000);
    scifStartTasksNbl(BV(SCIF_CAPACITIVE_TOUCH_TASK_ID));

    /* RF gen2 (identity + radio task) i app task (dispatch + przekaźnik) */
    radioTaskInit();
    appTaskInit();

    Display_printf(display, 0, 0, "up. touch=toggle, hold 4s=JOIN");

    /* Pętla obsługi dotyku: mierzy czas przytrzymania (własnym zegarem 20 ms).
     * Debounce: działamy na stanie STABILNYM przez DEBOUNCE_MS - pochłania flicker
     * cap-sense (chwilowe puszczenie w środku dotyku dawało podwójny toggle). */
    uint8_t  prev = 0;           /* poprzedni stan zdebounce'owany */
    uint8_t  rawPrev = 0;        /* poprzedni surowy stan (do liczenia stabilności) */
    uint8_t  dbState = 0;        /* aktualny stan zdebounce'owany */
    uint32_t stableMs = 0;
    uint32_t heldMs = 0;
    uint8_t  pairingArmed = 0;   /* ten hold już wystrzelił JOIN */
    uint8_t  pairing = 0;        /* miganie RED (czekamy na JOIN_ACCEPT) */
    uint32_t pairingMs = 0;

    while (1)
    {
        /* --- debounce surowego gTouchState -> dbState --- */
        uint8_t raw = (uint8_t)gTouchState;
        if (raw == rawPrev) { stableMs += POLL_MS; }
        else                { stableMs = 0; rawPrev = raw; }
        if (stableMs >= DEBOUNCE_MS) { dbState = raw; }
        uint8_t s = dbState;

        if (s && !prev) { heldMs = 0; pairingArmed = 0; }     /* start dotyku */
        if (s)
        {
            heldMs += POLL_MS;
            if (heldMs >= HOLD_JOIN_MS && !pairingArmed)
            {
                pairingArmed = 1;
                pairing      = 1;
                pairingMs    = 0;
                Display_printf(display, 0, 0, "[Touch] 4s hold -> JOIN (pairing)");
                rf_send_join();
            }
        }
        if (!s && prev)                                        /* puszczenie */
        {
            if (!pairingArmed && heldMs >= TOUCH_MIN_MS && heldMs < HOLD_JOIN_MS)
            {
                Display_printf(display, 0, 0, "[Touch] short -> relay toggle");
                app_relay_toggle();
            }
            heldMs = 0;
        }
        prev = s;

        /* Tryb parowania: RED miga aż node przyjmie adres (JOIN_ACCEPT) lub minie okno. */
        if (pairing)
        {
            pairingMs += POLL_MS;
            if (gNodeAddress != ADDR_UNPROVISIONED)
            {
                pairing = 0;
                GPIO_write(CONFIG_GPIO_RLED, 0);
                Display_printf(display, 0, 0, "[Pairing] JOIN_ACCEPT -> address 0x%02x, stop blink", gNodeAddress);
            }
            else if (pairingMs >= PAIRING_WINDOW_MS)
            {
                pairing = 0;
                GPIO_write(CONFIG_GPIO_RLED, 0);
                Display_printf(display, 0, 0, "[Pairing] window timeout, no JOIN_ACCEPT (still 0xFF)");
            }
            else if ((pairingMs % PAIRING_BLINK_MS) == 0)
            {
                GPIO_toggle(CONFIG_GPIO_RLED);
            }
        }

        usleep(POLL_MS * 1000);
    }
}
