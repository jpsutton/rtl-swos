#ifndef _LLDP_H_
#define _LLDP_H_

#include <stdint.h>

/*
 * LLDP (IEEE 802.1AB): announces the switch on every port and records the
 * neighbour heard on each one, for `show lldp neighbors`.
 *
 * `feature lldp` (or `lldp run`) turns it on: an LLDPDU goes out of every
 * port with a link every 30 s and when a link comes up, with a hold time
 * of 120 s. LLDPDUs reach the CPU through static L2 multicast entries for
 * 01:80:C2:00:00:0E, as LACPDUs do; with LLDP off the address is flooded
 * again, as by an unmanaged switch.
 */

#define LLDP_PORTS	10	/* indexed by logical port */
#define LLDP_TXT	24	/* chars kept of each text field, NUL included */
#define LLDP_TX_INTERVAL 30
#define LLDP_HOLD	120

struct lldp_nb {
	char chassis[LLDP_TXT];
	char port[LLDP_TXT];
	char sysname[LLDP_TXT];
	char portdesc[LLDP_TXT];
	char sysdesc[LLDP_TXT];
	uint8_t mgmt[4];
	uint16_t caps;		/* enabled capabilities */
	uint16_t ttl;		/* seconds left; 0 = no neighbour */
};

extern __xdata uint8_t lldp_enabled;
extern __xdata uint16_t lldp_no_tx, lldp_no_rx;	/* ports with lldp transmit/receive off */
extern __xdata struct lldp_nb lldp_nb[LLDP_PORTS];

void lldp_init(void) __banked;
void lldp_enable(__xdata uint8_t on) __banked;
void lldp_in(void) __banked;		/* uip_buf holds a frame to 01:80:C2:00:00:0E */
void lldp_tick(void) __banked;		/* 1 Hz while enabled */
void lldp_link_up(void) __banked;	/* announce soon on ports whose link came up */
void lldp_show(__xdata uint8_t detail) __banked;
/* The L2 entries steering LLDPDUs follow the VLAN database */
void lldp_fdb_refresh(void) __banked;

#endif
