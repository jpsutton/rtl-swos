/*
 * uIP TCP application dispatcher.
 *
 * uip.c (BANK1) invokes UIP_APPCALL directly, so tcp_appcall MUST be
 * reachable from BANK1 without a bank switch. It therefore carries NO
 * `#pragma codeseg`: it lands in the always-mapped CSEG, like
 * udp_apps.c's udp_callbacks(). The per-application handlers it calls
 * are __banked and switch to their own bank through the trampoline.
 *
 * The telnet server is the only TCP application; anything else is
 * aborted.
 */
#include "uip/uip.h"
#include "telnetd.h"
#include "tcp_app.h"

void tcp_appcall(void)
{
	if (uip_conn->lport == HTONS(TELNET_PORT))
		telnetd_appcall();
	else
		uip_abort();
}
