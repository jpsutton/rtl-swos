#ifndef __SWCFG_H__
#define __SWCFG_H__

#include <stdint.h>

/*
 * Switch configuration state model.
 *
 * The hardware stores VLAN membership per VLAN (member mask + tagged
 * mask), while the configuration language is per interface
 * (`switchport access vlan 10`). This module holds the per-interface
 * state and the VLAN database, and sw_apply() recomputes every VLAN's
 * masks plus each port's PVID and ingress mode from it.
 *
 * Ports are indexed by LOGICAL port number (machine.min_port ..
 * machine.max_port), the same numbering the drivers use.
 */

#define SW_MAX_VLANS	32	/* VLAN database capacity */
#define SW_MAX_RANGES	8	/* VID ranges per trunk allowed list */
#define SW_NPORTS	9	/* logical ports 0..8 */

#define SW_MODE_ACCESS	0
#define SW_MODE_TRUNK	1

/* sw_allowed_edit() operations */
#define SW_AL_SET	1	/* replace with the list */
#define SW_AL_ADD	2
#define SW_AL_REMOVE	3
#define SW_AL_ALL	4
#define SW_AL_NONE	5

/* Result codes */
#define SW_OK		0
#define SW_ERR_FULL	1	/* database / range table / name table full */
#define SW_ERR_RANGE	2	/* VID or value out of range */
#define SW_ERR_VLAN1	3	/* VLAN 1 cannot be deleted */
#define SW_ERR_SYNTAX	4	/* malformed VLAN list */

#define SW_VID_MAX	4094

struct sw_range {
	uint16_t lo, hi;
};

struct sw_port {
	uint8_t mode;
	uint16_t access_vid;
	uint16_t native_vid;
	uint8_t nranges;
	struct sw_range allowed[SW_MAX_RANGES];
	/* Settings the PHY cannot report back, shadowed for the running
	 * config. The MTU is read back from its register instead. */
	uint8_t speed;		/* PHY_SPEED_* as configured */
	uint8_t shut;		/* administratively down */
	uint8_t eee_off;	/* no power efficient-ethernet (on by default) */
	uint8_t prot;		/* switchport protected */
	uint32_t rl_in;		/* rate-limit input, kbit/s; 0 = none */
	uint32_t rl_out;	/* rate-limit output, kbit/s; 0 = none */
	uint8_t rl_in_drop;	/* input limit drops instead of pausing */
	uint8_t duplex;		/* PHY_DUPLEX_*; BOTH = auto */
};

/* SPAN session 1 (the hardware has one) */
#define SW_MON_NONE	0xff
extern __xdata uint8_t sw_mon_dst;	/* logical port, SW_MON_NONE = unset */
extern __xdata uint16_t sw_mon_rx, sw_mon_tx;	/* source port masks */

#define SW_RATE_MIN	16UL		/* kbit/s, also the step */
#define SW_RATE_MAX	10000000UL

extern __xdata uint16_t sw_vlans[SW_MAX_VLANS];	/* 0 = free slot */
extern __xdata struct sw_port sw_ports[SW_NPORTS];
extern __xdata uint8_t sw_igmp;		/* ip igmp snooping */
extern __xdata uint8_t sw_mac_boot[6];	/* management MAC as read at boot */

void sw_init(void) __banked;
/* The port's counters (as port_counters_get()) since the last clear */
void sw_counters_get(uint8_t lport, __xdata uint32_t * __xdata c) __banked;
/* clear counters: ports is a logical port mask */
void sw_counters_clear(__xdata uint16_t ports) __banked;
uint8_t sw_vlan_exists(uint16_t vid) __banked;
uint8_t sw_vlan_add(uint16_t vid) __banked;
uint8_t sw_vlan_del(uint16_t vid) __banked;
/* name == 0 removes the name */
uint8_t sw_vlan_name_set(uint16_t vid, __xdata const char * __xdata name) __banked;
uint8_t sw_allowed_edit(uint8_t lport, __xdata uint8_t op, __xdata const char * __xdata list) __banked;
uint8_t sw_port_allows(uint8_t lport, __xdata uint16_t vid) __banked;
/* Push the whole state to the hardware. While deferred (boot-time config
 * replay) it only records that a push is due; sw_defer(0) then pushes
 * once. */
void sw_apply(void) __banked;
void sw_defer(uint8_t on) __banked;

void sw_mtu_set(uint8_t lport, __xdata uint16_t mtu) __banked;
void sw_eee_apply(uint8_t lport) __banked;
void sw_protect_apply(void) __banked;
void sw_rate_apply(uint8_t lport) __banked;
void sw_mon_apply(void) __banked;
/* Move a port into link aggregation group 1-4, or out of any (0) */
void sw_lag_join(uint8_t lport, __xdata uint8_t lag) __banked;

/* Management interface (the single SVI). ip/mask as A<<24|B<<16|C<<8|D. */
void sw_mgmt_vlan_set(uint16_t vid) __banked;
void sw_mgmt_ip_set(uint32_t ip, __xdata uint32_t mask) __banked;
void sw_mgmt_dhcp(void) __banked;
void sw_gateway_set(uint32_t gw) __banked;
/* aabb.ccdd.eeff, aa:bb:cc:dd:ee:ff or aa-bb-...; 0 on a syntax error */
uint8_t sw_mac_parse(__xdata const char *str, __xdata uint8_t * __xdata mac) __banked;
/* 0 unless unicast and globally administered (sw_mac_boot is exempt) */
uint8_t sw_mgmt_mac_set(__xdata const uint8_t *mac) __banked;

/* Remote syslog; port 0 selects the default 514 */
void sw_logging_host(uint32_t ip, __xdata uint16_t port) __banked;
void sw_logging_off(void) __banked;

#endif
