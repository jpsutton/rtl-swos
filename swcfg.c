/*
 * Switch configuration state model. See swcfg.h.
 *
 * Locals and non-first parameters live in xdata: the 8051 overlay
 * segment (internal RAM) is already exhausted, see cli.c.
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_sfr.h"
#include "rtl837x_port.h"
#include "rtl837x_phy.h"
#include "rtl837x_bandwidth.h"
#include "phy.h"
#include "machine.h"
#include "dhcp.h"
#include "syslog.h"
#include "uip/uip.h"
#include "swcfg.h"

#include "lacp.h"
#include "lldp.h"

#pragma codeseg BANK3
#pragma constseg BANK3

extern __code const struct machine machine;
extern __xdata struct machine_runtime machine_detected;
extern __xdata uint16_t management_vlan;
extern __xdata uint16_t vlan_ptr;
extern __xdata uint8_t vlan_names[VLAN_NAMES_SIZE];
extern __code const uint8_t * __code const hex;
extern __xdata struct dhcp_state dhcp_state;

__xdata uint16_t sw_vlans[SW_MAX_VLANS];
__xdata struct sw_port sw_ports[SW_NPORTS];
__xdata uint8_t sw_igmp;
__xdata uint8_t sw_mac_boot[6];
__xdata uint16_t sw_hidden_vid[SW_NPORTS];
static __xdata uint16_t sw_hidden_old[SW_NPORTS];
__xdata uint8_t sw_mon_dst;
__xdata uint16_t sw_mon_rx, sw_mon_tx;
static __xdata uint8_t sw_deferred, sw_dirty;

#define SW_NAME_MAX 32

/* 4096-bit VID scratch for the allowed-list set operations */
static __xdata uint8_t sw_bm[512];
#define BM_SET(v) (sw_bm[(v) >> 3] |= (uint8_t)(1 << ((v) & 7)))
#define BM_CLR(v) (sw_bm[(v) >> 3] &= (uint8_t)~(1 << ((v) & 7)))
#define BM_GET(v) ((sw_bm[(v) >> 3] >> ((v) & 7)) & 1)


/* clear counters: what the counters read then, subtracted from now on */
static __xdata uint32_t sw_cnt_base[SW_NPORTS][4];

void sw_counters_get(uint8_t lport, __xdata uint32_t * __xdata c) __banked
{
	static __xdata uint8_t lp, k;
	static __xdata uint32_t * __xdata cp;

	lp = lport;
	cp = c;
	port_counters_get(lp, cp);
	for (k = 0; k < 4; k++)
		cp[k] -= sw_cnt_base[lp][k];
}


void sw_counters_clear(__xdata uint16_t ports) __banked
{
	static __xdata uint8_t lp;

	for (lp = 0; lp < SW_NPORTS; lp++)
		if (ports & ((uint16_t)1 << lp))
			port_counters_get(lp, sw_cnt_base[lp]);
}


void sw_init(void) __banked
{
	static __xdata uint8_t k;

	for (k = 0; k < SW_MAX_VLANS; k++)
		sw_vlans[k] = 0;
	sw_vlans[0] = 1;
	for (k = 0; k < SW_NPORTS; k++) {
		sw_hidden_vid[k] = 0;
		sw_ports[k].mode = SW_MODE_ACCESS;
		sw_ports[k].access_vid = 1;
		sw_ports[k].native_vid = 1;
		sw_ports[k].nranges = 1;
		sw_ports[k].allowed[0].lo = 1;
		sw_ports[k].allowed[0].hi = SW_VID_MAX;
		sw_ports[k].speed = PHY_SPEED_AUTO;
		sw_ports[k].shut = 0;
		sw_ports[k].eee_off = 0;
		sw_ports[k].prot = 0;
		sw_ports[k].rl_in = 0;
		sw_ports[k].rl_out = 0;
		sw_ports[k].rl_in_drop = 0;
		sw_ports[k].duplex = PHY_DUPLEX_BOTH;
	}
	for (k = 0; k < 6; k++)	/* read from flash or derived before this */
		sw_mac_boot[k] = uip_ethaddr.addr[k];
	sw_mon_dst = SW_MON_NONE;
	sw_mon_rx = 0;
	sw_mon_tx = 0;
	sw_igmp = 0;
	sw_deferred = 0;
	sw_dirty = 0;
	lacp_init();
	lldp_init();
	/* vlan_setup() leaves every port accepting all frames; push the
	 * access-port defaults now, a config without switchport lines never
	 * would. */
	sw_apply();
}


uint8_t sw_vlan_exists(uint16_t vid) __banked
{
	static __xdata uint8_t k;

	if (!vid)
		return 0;
	for (k = 0; k < SW_MAX_VLANS; k++) {
		if (sw_vlans[k] == vid)
			return 1;
	}
	return 0;
}


uint8_t sw_vlan_add(uint16_t vid) __banked
{
	static __xdata uint8_t k;

	if (!vid || vid > SW_VID_MAX)
		return SW_ERR_RANGE;
	if (sw_vlan_exists(vid))
		return SW_OK;
	for (k = 0; k < SW_MAX_VLANS; k++) {
		if (!sw_vlans[k]) {
			sw_vlans[k] = vid;
			return SW_OK;
		}
	}
	return SW_ERR_FULL;
}


uint8_t sw_vlan_del(uint16_t vid) __banked
{
	static __xdata uint8_t k;

	if (vid == 1)
		return SW_ERR_VLAN1;
	for (k = 0; k < SW_MAX_VLANS; k++) {
		if (sw_vlans[k] == vid) {
			sw_vlans[k] = 0;
			vlan_delete(vid);	/* also drops the name */
			sw_apply();		/* a hidden VLAN may move up */
			return SW_OK;
		}
	}
	return SW_OK;	/* not configured: nothing to do */
}


/* Names share the packed "VVVname " table with the legacy vlan command */
uint8_t sw_vlan_name_set(uint16_t vid, __xdata const char * __xdata name) __banked
{
	static __xdata uint8_t n, k;
	static __xdata uint8_t * __xdata d;

	vlan_name_remove(vid);
	if (!name || !*name)
		return SW_OK;
	for (n = 0; name[n] && name[n] != ' ' && n < SW_NAME_MAX; n++)
		;
	if (vlan_ptr + n + 5 > VLAN_NAMES_SIZE)
		return SW_ERR_FULL;
	d = &vlan_names[vlan_ptr];
	*d++ = hex[(vid >> 8) & 0xf];
	*d++ = hex[(vid >> 4) & 0xf];
	*d++ = hex[vid & 0xf];
	for (k = 0; k < n; k++)
		*d++ = name[k];
	*d++ = ' ';
	*d = 0;
	vlan_ptr = d - vlan_names;
	return SW_OK;
}


static void bm_clear(void)
{
	static __xdata uint16_t k;

	for (k = 0; k < sizeof(sw_bm); k++)
		sw_bm[k] = 0;
}


static void bm_load(__xdata uint8_t lp)
{
	static __xdata uint8_t r;
	static __xdata uint16_t v;

	bm_clear();
	for (r = 0; r < sw_ports[lp].nranges; r++) {
		for (v = sw_ports[lp].allowed[r].lo; v <= sw_ports[lp].allowed[r].hi; v++)
			BM_SET(v);
	}
}


/* Compress the bitmap back into the port's range table. Leaves the
 * port untouched when it needs more than SW_MAX_RANGES ranges. */
static uint8_t bm_store(__xdata uint8_t lp)
{
	static __xdata uint16_t v, start;
	static __xdata uint8_t n, inrun, b;

	n = 0;
	inrun = 0;
	for (v = 1; v <= SW_VID_MAX; v++) {
		if (BM_GET(v)) {
			if (!inrun) {
				n++;
				inrun = 1;
			}
		} else {
			inrun = 0;
		}
	}
	if (n > SW_MAX_RANGES)
		return SW_ERR_FULL;

	n = 0;
	inrun = 0;
	for (v = 1; v <= SW_VID_MAX + 1; v++) {
		b = (v <= SW_VID_MAX) ? BM_GET(v) : 0;
		if (b && !inrun) {
			start = v;
			inrun = 1;
		} else if (!b && inrun) {
			sw_ports[lp].allowed[n].lo = start;
			sw_ports[lp].allowed[n].hi = v - 1;
			n++;
			inrun = 0;
		}
	}
	sw_ports[lp].nranges = n;
	return SW_OK;
}


/* "10,20-30,40": set (on) or clear each listed VID in the bitmap. The
 * port is only updated by the caller on success, so a malformed list
 * changes nothing. */
static uint8_t bm_parse(__xdata const char * __xdata s, __xdata uint8_t on)
{
	static __xdata const char * __xdata p;
	static __xdata uint16_t a, b, v;
	static __xdata uint8_t digits;

	p = s;
	while (1) {
		a = 0;
		digits = 0;
		while (*p >= '0' && *p <= '9') {
			a = a * 10 + (*p - '0');
			p++;
			if (++digits > 4)
				return SW_ERR_SYNTAX;
		}
		if (!digits)
			return SW_ERR_SYNTAX;
		b = a;
		if (*p == '-') {
			p++;
			b = 0;
			digits = 0;
			while (*p >= '0' && *p <= '9') {
				b = b * 10 + (*p - '0');
				p++;
				if (++digits > 4)
					return SW_ERR_SYNTAX;
			}
			if (!digits)
				return SW_ERR_SYNTAX;
		}
		if (a < 1 || b > SW_VID_MAX || a > b)
			return SW_ERR_RANGE;
		for (v = a; v <= b; v++) {
			if (on)
				BM_SET(v);
			else
				BM_CLR(v);
		}
		if (*p == ',') {
			p++;
			continue;
		}
		if (*p == 0 || *p == ' ')
			return SW_OK;
		return SW_ERR_SYNTAX;
	}
}


uint8_t sw_allowed_edit(uint8_t lport, __xdata uint8_t op, __xdata const char * __xdata list) __banked
{
	static __xdata uint8_t rc, lp;

	lp = lport;
	if (lp >= SW_NPORTS)
		return SW_ERR_RANGE;
	switch (op) {
	case SW_AL_ALL:
		sw_ports[lp].nranges = 1;
		sw_ports[lp].allowed[0].lo = 1;
		sw_ports[lp].allowed[0].hi = SW_VID_MAX;
		return SW_OK;
	case SW_AL_NONE:
		sw_ports[lp].nranges = 0;
		return SW_OK;
	case SW_AL_SET:
		bm_clear();
		rc = bm_parse(list, 1);
		break;
	case SW_AL_ADD:
		bm_load(lp);
		rc = bm_parse(list, 1);
		break;
	case SW_AL_REMOVE:
		bm_load(lp);
		rc = bm_parse(list, 0);
		break;
	default:
		return SW_ERR_SYNTAX;
	}
	if (rc != SW_OK)
		return rc;
	return bm_store(lp);
}


uint8_t sw_port_allows(uint8_t lport, __xdata uint16_t vid) __banked
{
	static __xdata uint8_t r, lp;

	lp = lport;
	for (r = 0; r < sw_ports[lp].nranges; r++) {
		if (vid >= sw_ports[lp].allowed[r].lo && vid <= sw_ports[lp].allowed[r].hi)
			return 1;
	}
	return 0;
}


/* Recompute every configured VLAN's member/tagged masks, then each
 * port's PVID and ingress acceptance, from the per-port state. */
void sw_apply(void) __banked
{
	static __xdata uint8_t k, p;
	static __xdata uint16_t vid, bit, members, tagged, hid;
	static __xdata struct sw_port * __xdata sp;

	if (sw_deferred) {
		sw_dirty = 1;
		return;
	}
	sw_dirty = 0;
	for (k = 0; k < SW_MAX_VLANS; k++) {
		vid = sw_vlans[k];
		if (!vid)
			continue;
		members = 0;
		tagged = 0;
		for (p = machine.min_port; p <= machine.max_port; p++) {
			sp = &sw_ports[p];
			bit = (uint16_t)1 << p;
			if (sp->mode == SW_MODE_ACCESS) {
				if (sp->access_vid == vid)
					members |= bit;
			} else if (sw_port_allows(p, vid)) {
				members |= bit;
				if (sp->native_vid != vid)
					tagged |= bit;
			}
		}
		vlan_settings.vlan = vid;
		vlan_settings.members = members;
		vlan_settings.tagged = tagged;
		vlan_create();
	}
	hid = SW_VID_MAX + 1;
	for (p = 0; p < SW_NPORTS; p++) {
		sw_hidden_old[p] = sw_hidden_vid[p];
		sw_hidden_vid[p] = 0;
	}
	for (p = machine.min_port; p <= machine.max_port; p++) {
		sp = &sw_ports[p];
		if (sp->mode == SW_MODE_ACCESS) {
			port_pvid_set(p, sp->access_vid);
			port_ingress_filter(p, VLAN_UNTAGGED);
		} else if (sw_port_allows(p, sp->native_vid)) {
			port_pvid_set(p, sp->native_vid);
			port_ingress_filter(p, VLAN_ALL);
		} else {
			/* No untagged traffic: only the CPU hears the hidden
			 * VLAN, see swcfg.h */
			do
				hid--;
			while (sw_vlan_exists(hid));
			sw_hidden_vid[p] = hid;
			vlan_settings.vlan = hid;
			vlan_settings.members = (uint16_t)1 << p;
			vlan_settings.tagged = 0;
			vlan_create();
			port_pvid_set(p, hid);
			port_ingress_filter(p, VLAN_ALL);
		}
	}
	/* Hidden VLANs no port uses any more, unless now configured */
	for (k = 0; k < SW_NPORTS; k++) {
		vid = sw_hidden_old[k];
		if (!vid || sw_vlan_exists(vid))
			continue;
		for (p = 0; p < SW_NPORTS; p++)
			if (sw_hidden_vid[p] == vid)
				break;
		if (p == SW_NPORTS)
			vlan_delete(vid);
	}
	if (lacp_ports)
		lacp_fdb_refresh();	/* LACPDUs arrive in the PVIDs */
	if (lldp_enabled)
		lldp_fdb_refresh();
}


void sw_l2mc_set(uint8_t mac_last, __xdata uint16_t pmask) __banked
{
	static __xdata uint8_t k, m;

	m = mac_last;
	for (k = 0; k < SW_MAX_VLANS; k++)
		if (sw_vlans[k])
			port_l2mc_set(m, sw_vlans[k], pmask);
	for (k = 0; k < SW_NPORTS; k++)
		if (sw_hidden_vid[k])
			port_l2mc_set(m, sw_hidden_vid[k], pmask);
}


void sw_defer(uint8_t on) __banked
{
	sw_deferred = on;
	if (!on && sw_dirty)
		sw_apply();
}


void sw_mtu_set(uint8_t lport, __xdata uint16_t mtu) __banked
{
	/* Same encoding as the legacy `mtu` command */
	REG_WRITE(RTL8373_REG_MAC_L2_PORT_MAX_LEN + ((uint16_t)lport << 8),
		  (mtu >> 10) & 0xf, (mtu >> 2) & 0xff,
		  ((mtu & 0x3) << 6) | ((mtu >> 8) & 0x3f), mtu & 0xff);
}


static __xdata uint8_t ipb[4];

static void ip_split(__xdata uint32_t v)
{
	ipb[0] = v >> 24;
	ipb[1] = (v >> 16) & 0xff;
	ipb[2] = (v >> 8) & 0xff;
	ipb[3] = v & 0xff;
}


void sw_mgmt_vlan_set(uint16_t vid) __banked
{
	static __xdata uint16_t v;

	v = vid;
	if (v == management_vlan)
		return;
	port_l2_static_mgmt(uip_ethaddr.addr, management_vlan, true);
	management_vlan = v;
	port_l2_static_mgmt(uip_ethaddr.addr, management_vlan, false);
}


void sw_mgmt_ip_set(uint32_t ip, __xdata uint32_t mask) __banked
{
	static __xdata uint32_t m;

	m = mask;
	if (dhcp_state.state)
		dhcp_stop();
	ip_split(ip);
	uip_ipaddr(&uip_hostaddr, ipb[0], ipb[1], ipb[2], ipb[3]);
	ip_split(m);
	uip_ipaddr(&uip_netmask, ipb[0], ipb[1], ipb[2], ipb[3]);
}


void sw_mgmt_dhcp(void) __banked
{
	dhcp_start();
}


void sw_gateway_set(uint32_t gw) __banked
{
	ip_split(gw);
	uip_ipaddr(&uip_draddr, ipb[0], ipb[1], ipb[2], ipb[3]);
}


void sw_logging_host(uint32_t ip, __xdata uint16_t port) __banked
{
	static __xdata uint16_t pt;

	pt = port;
	if (syslog_state.enabled)
		syslog_stop();
	ip_split(ip);
	syslog_state.server_ip[0] = ipb[0];
	syslog_state.server_ip[1] = ipb[1];
	syslog_state.server_ip[2] = ipb[2];
	syslog_state.server_ip[3] = ipb[3];
	syslog_state.server_port = pt ? pt : SYSLOG_PORT_DEFAULT;
	syslog_start();
}


void sw_logging_off(void) __banked
{
	if (syslog_state.enabled)
		syslog_stop();
}


/* EEE is enabled on every port at init with the chip's top speed */
void sw_eee_apply(uint8_t lport) __banked
{
	static __xdata uint8_t lp;

	lp = lport;
	if (sw_ports[lp].eee_off)
		port_eee_disable(lp);
	else
		port_eee_enable(lp, machine_detected.isRTL8373 ? EEE_10G : EEE_2G5);
}


/* Protected ports may not forward to each other; every other pair may.
 * The hardware takes one egress mask per port, CPU port included. */
void sw_protect_apply(void) __banked
{
	static __xdata uint8_t p;
	static __xdata uint16_t all, prot, m;

	all = 0x200;
	prot = 0;
	for (p = machine.min_port; p <= machine.max_port; p++) {
		all |= (uint16_t)1 << p;
		if (sw_ports[p].prot)
			prot |= (uint16_t)1 << p;
	}
	for (p = machine.min_port; p <= machine.max_port; p++) {
		m = all;
		if (sw_ports[p].prot)
			m = (all & ~prot) | ((uint16_t)1 << p);
		port_isolate(p, m);
	}
}


void sw_rate_apply(uint8_t lport) __banked
{
	static __xdata uint8_t lp;

	lp = lport;
	if (sw_ports[lp].rl_in) {
		bandwidth_ingress_set(lp, sw_ports[lp].rl_in);	/* pauses by default */
		if (sw_ports[lp].rl_in_drop)
			bandwidth_ingress_drop(lp);
	} else {
		bandwidth_ingress_disable(lp);
	}
	if (sw_ports[lp].rl_out)
		bandwidth_egress_set(lp, sw_ports[lp].rl_out);
	else
		bandwidth_egress_disable(lp);
}


void sw_mon_apply(void) __banked
{
	if (sw_mon_dst != SW_MON_NONE && (sw_mon_rx | sw_mon_tx))
		port_mirror_set(sw_mon_dst, sw_mon_rx, sw_mon_tx);
	else
		port_mirror_del();
}


void sw_lag_join(uint8_t lport, __xdata uint8_t lag) __banked
{
	static __xdata uint8_t g, lp;
	static __xdata uint16_t m, bit;

	lp = lport;
	bit = (uint16_t)1 << lp;
	for (g = 0; g < 4; g++) {
		m = port_lag_members_get(g);
		if (g + 1 == lag) {
			if (!(m & bit))
				port_lag_members_set(g, m | bit);
		} else if (m & bit) {
			port_lag_members_set(g, m & ~bit);
		}
	}
}


static uint8_t hexval(__xdata char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c |= 0x20;
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return 0xff;
}


uint8_t sw_mac_parse(__xdata const char *str, __xdata uint8_t * __xdata mac) __banked
{
	static __xdata const char * __xdata p;
	static __xdata uint8_t n, h, nib;

	p = str;
	n = 0;		/* nibbles taken */
	while (*p && *p != ' ' && n < 12) {
		if (*p == ':' || *p == '-' || *p == '.') {
			p++;
			continue;
		}
		h = hexval(*p++);
		if (h == 0xff)
			return 0;
		nib = n & 1;
		if (!nib)
			mac[n >> 1] = h << 4;
		else
			mac[n >> 1] |= h;
		n++;
	}
	return n == 12 && (!*p || *p == ' ');
}


uint8_t sw_mgmt_mac_set(__xdata const uint8_t *mac) __banked
{
	static __xdata const uint8_t * __xdata m;
	static __xdata uint8_t k;

	m = mac;
	/* A user-supplied MAC must be unicast and globally administered.
	 * The boot MAC is exempt: without one in flash the firmware derives a
	 * locally administered address, and `no mac-address` restores it. */
	if (m != sw_mac_boot && ((m[0] & 0x03) || !(m[0] | m[1] | m[2])))
		return 0;
	/* swap the static management entry over to the new address */
	port_l2_static_mgmt(uip_ethaddr.addr, management_vlan, true);
	for (k = 0; k < 6; k++)
		uip_ethaddr.addr[k] = m[k];
	port_l2_static_mgmt(uip_ethaddr.addr, management_vlan, false);
	return 1;
}
