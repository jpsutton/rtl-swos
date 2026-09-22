#ifndef __TCP_APP_H__
#define __TCP_APP_H__

/* Since this file will be included by uip.h, we cannot include uip.h
   here. But we might need to include uipopt.h if we need the u8_t and
   u16_t datatypes. */
#include "uipopt.h"

/* The uip_tcp_appstate_t datatype: per-connection application state,
   allocated together with each TCP connection. */
typedef struct tcp_app_state {
   uint8_t tstate;
} uip_tcp_appstate_t;

/* The application function called by uIP on every TCP/IP event.
 * Dispatches by local port; the telnet server is the only TCP
 * application. */
void tcp_appcall(void);
#ifndef UIP_APPCALL
#define UIP_APPCALL tcp_appcall
#endif /* UIP_APPCALL */

#endif
