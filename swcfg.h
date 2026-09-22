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
};

extern __xdata uint16_t sw_vlans[SW_MAX_VLANS];	/* 0 = free slot */
extern __xdata struct sw_port sw_ports[SW_NPORTS];

void sw_init(void) __banked;
uint8_t sw_vlan_exists(uint16_t vid) __banked;
uint8_t sw_vlan_add(uint16_t vid) __banked;
uint8_t sw_vlan_del(uint16_t vid) __banked;
/* name == 0 removes the name */
uint8_t sw_vlan_name_set(uint16_t vid, __xdata const char * __xdata name) __banked;
uint8_t sw_allowed_edit(uint8_t lport, __xdata uint8_t op, __xdata const char * __xdata list) __banked;
uint8_t sw_port_allows(uint8_t lport, __xdata uint16_t vid) __banked;
/* Push the whole state to the hardware */
void sw_apply(void) __banked;

void sw_mtu_set(uint8_t lport, __xdata uint16_t mtu) __banked;

/* Management interface (the single SVI). ip/mask as A<<24|B<<16|C<<8|D. */
void sw_mgmt_vlan_set(uint16_t vid) __banked;
void sw_mgmt_ip_set(uint32_t ip, __xdata uint32_t mask) __banked;
void sw_mgmt_dhcp(void) __banked;
void sw_gateway_set(uint32_t gw) __banked;

/* Remote syslog; port 0 selects the default 514 */
void sw_logging_host(uint32_t ip, __xdata uint16_t port) __banked;
void sw_logging_off(void) __banked;

#endif
