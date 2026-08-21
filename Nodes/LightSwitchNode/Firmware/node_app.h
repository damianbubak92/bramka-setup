/*
 * node_app.h - warstwa aplikacji LightSwitch: dispatch komend z bramki + przekaźnik.
 */
#ifndef NODE_APP_H
#define NODE_APP_H

/* Start app-taska (kolejka komend + event). Woła main po radioTaskInit(). */
void appTaskInit(void);

/* Lokalne przełączenie przekaźnika światła (krótki dotyk pada) + raport stanu do bramki. */
void app_relay_toggle(void);

#endif /* NODE_APP_H */
