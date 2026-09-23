#ifndef _LACP_H_
#define _LACP_H_

#include <stdint.h>

/*
 * LACP (IEEE 802.1AX / 802.3ad) for the port-channels.
 *
 * The ASIC aggregates statically: a port-channel is a member mask
 * (port_lag_members_set()). LACP runs on the 8051 and decides which of the
 * ports configured into a port-channel with `mode active|passive` are in
 * that mask: a port joins once both ends agree (actor and partner in sync)
 * and leaves when the partner times out, its link goes down or its partner
 * turns out to be a different system. A port that is not bundled keeps
 * forwarding as an individual port.
 *
 * LACPDUs (01:80:C2:00:00:02) reach the CPU the way BPDUs do, through a
 * static L2 multicast entry per PVID in use (lacp_fdb_refresh()).
 */

#define LACP_PORTS	10	/* indexed by logical port */

/* ->lo of the `channel-group N mode X` literals; a bare `channel-group N`
 * carries the number node's lo (1) and means `on` */
#define LACP_MODE_ON		0x10
#define LACP_MODE_ACTIVE	0x11
#define LACP_MODE_PASSIVE	0x12

/* Actor/partner state bits of an LACPDU */
#define LACP_ST_ACTIVITY	0x01
#define LACP_ST_TIMEOUT		0x02	/* short timeout (fast rate) */
#define LACP_ST_AGGREGATION	0x04
#define LACP_ST_SYNC		0x08
#define LACP_ST_COLLECTING	0x10
#define LACP_ST_DISTRIBUTING	0x20
#define LACP_ST_DEFAULTED	0x40
#define LACP_ST_EXPIRED		0x80

/* Configuration, per logical port */
extern __xdata uint8_t lacp_group[LACP_PORTS];	/* port-channel 1-4, 0 = not an LACP port */
extern __xdata uint8_t lacp_mode[LACP_PORTS];	/* LACP_MODE_ACTIVE / _PASSIVE */
extern __xdata uint8_t lacp_fast[LACP_PORTS];	/* lacp rate fast */
extern __xdata uint16_t lacp_pprio[LACP_PORTS];	/* lacp port-priority, default 32768 */
extern __xdata uint16_t lacp_sysprio;		/* lacp system-priority, default 32768 */
extern __xdata uint8_t lacp_minlinks[4];	/* lacp min-links per port-channel, default 1 */

/* Runtime */
extern __xdata uint8_t lacp_actor_st[LACP_PORTS];
extern __xdata uint8_t lacp_partner_st[LACP_PORTS];
extern __xdata uint16_t lacp_bundled;		/* ports LACP put into their port-channel */
extern __xdata uint16_t lacp_ports;		/* ports with LACP configured */

void lacp_init(void) __banked;
/* Put a port into port-channel `group` (1-4) with `mode`, or take it out
 * of LACP (group 0). The caller keeps static and LACP members apart. */
void lacp_port_set(uint8_t lport, __xdata uint8_t group, __xdata uint8_t mode) __banked;
/* Settings that change what the port announces: send an LACPDU soon */
void lacp_port_changed(uint8_t lport) __banked;
/* uip_buf holds a frame to 01:80:C2:00:00:02; consumes it */
void lacp_in(void) __banked;
/* 10 Hz: timers, link supervision, bundling, transmission */
void lacp_tick(void) __banked;
/* Steer LACPDUs to the CPU in every PVID an LACP port uses */
void lacp_fdb_refresh(void) __banked;
/* show lacp [neighbor] */
void lacp_show(void) __banked;

#define LACP_TICK_HZ	10

#endif
