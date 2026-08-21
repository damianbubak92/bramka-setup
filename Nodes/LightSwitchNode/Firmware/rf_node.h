/*
 * rf_node.h - publiczny interfejs warstwy RF (rfEchoTx.c) dla aplikacji.
 *
 * RF task (radioTaskInit) obsługuje ramki gen2 'E'+factory_id, JOIN, ACK i downlink;
 * odebrane komendy trafiają do kolejki aplikacji (solarControllerQueue). Aplikacja
 * odsyła raporty stanu wrzucając MessageStruct do radioQueue + Event_post(SEND_PACKET).
 */
#ifndef RF_NODE_H
#define RF_NODE_H

#include <mqueue.h>
#include <ti/sysbios/knl/Event.h>

/* Start warstwy RF (czyta identity, otwiera kolejkę, tworzy radio task). Woła main. */
void radioTaskInit(void);

/* Wyślij JOIN REQUEST (src 0xFF + factory_id). Woła aplikacja po 4 s hold touch pada. */
void rf_send_join(void);

/* Kanał uplink do RF task: aplikacja wrzuca MessageStruct + posta EVENT_SEND_PACKET. */
extern mqd_t        radioQueue;
extern Event_Handle radioEventHandle;
#define EVENT_SEND_PACKET   (1 << 0)

#endif /* RF_NODE_H */
