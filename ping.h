#ifndef _PING_H_
#define _PING_H_

#include <stdint.h>

/*
 * ping: ICMP echo requests from the management interface, in the style of
 * IOS (`!` per reply, `.` per timeout, a summary line).
 *
 * A command cannot wait for the network: over telnet it runs inside the
 * stack's own callback. So `ping` only starts the run; ping_tick() in the
 * main loop sends, waits and prints, into the session that started it,
 * and that session's prompt is held back until the run ends. Any key (or
 * ^C) in that session aborts it.
 */

#define PING_IDLE	0
#define PING_RESOLVE	1
#define PING_SEND	2
#define PING_WAIT	3

extern __xdata uint8_t ping_phase;	/* PING_*: nonzero while a run is on */
extern __xdata uint8_t ping_owner;	/* CLI_CONSOLE or CLI_VTY */

/* Start a run: host (name or dotted address) is the word at `host`;
 * count echos of `size` bytes (IP datagram). Prints the header. */
void ping_start(__xdata char * __xdata host, __xdata uint16_t count, __xdata uint16_t size) __banked;
void ping_abort(void) __banked;
void ping_tick(void) __banked;		/* main loop, while ping_phase */
void ping_reply(void) __banked;		/* uIP: an ICMP echo reply is in uip_buf */

#endif
