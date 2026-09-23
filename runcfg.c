/*
 * Running configuration renderer. See runcfg.h.
 *
 * One emitter feeds two sinks: the console (show running-config) and
 * cfg_buf (write memory). Everything is read from the live state, so a
 * setting made by a legacy command that still runs through the old
 * parser is rendered as well; only settings without any readable state
 * (speed, shutdown, IGMP snooping) come from the swcfg shadows.
 *
 * IPv4 addresses are read byte-wise: uIP keeps them in network order in
 * memory. uIP's own uip_ipaddrN() accessors call htons(), which lives in
 * BANK1 and is not __banked, so they must not be used from here.
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_flash.h"
#include "rtl837x_phy.h"
#include "phy.h"
#include "rtl837x_port.h"
#include "machine.h"
#include "dhcp.h"
#include "syslog.h"
#include "telnetd.h"
#include "tftp.h"
#include "uip/uip.h"
#include "swcfg.h"
#include "rtl837x_stp.h"
#include "rtl837x_igmp.h"
#include "lacp.h"
#include "runcfg.h"

#pragma codeseg BANK3
#pragma constseg BANK3

extern __code const struct machine machine;
extern __xdata uint16_t management_vlan;
extern __xdata uint8_t vlan_names[VLAN_NAMES_SIZE];
extern __xdata struct dhcp_state dhcp_state;
extern __xdata char passwd[21];
extern __xdata uint8_t sfr_data[4];
extern __xdata uint8_t flash_buf[FLASH_BUF_SIZE];
extern __xdata struct flash_region_t flash_region;
extern __xdata bool stp_enabled;

/* Scratch only: filled before every use, so it may live above XRAM_LOW_LIMIT */
__xdata __at(XRAM_CFG_BUF) uint8_t cfg_buf[CONFIG_LEN];

static __xdata uint8_t rc_tobuf;	/* 1: render into cfg_buf */
static __xdata uint16_t rc_len;
static __xdata uint8_t rc_over;

static void rc_c(__xdata char c)
{
	if (!rc_tobuf) {
		write_char(c);
		return;
	}
	if (rc_len < CONFIG_LEN - 1)
		cfg_buf[rc_len++] = c;
	else
		rc_over = 1;
}


static void rc_s(__code const char * __xdata s)
{
	while (*s)
		rc_c(*s++);
}


/* xdata string up to NUL, or up to the first space when word is set
 * (the vlan name table separates entries with a space) */
static void rc_x(__xdata const char * __xdata s, __xdata uint8_t word)
{
	while (*s && !(word && *s == ' '))
		rc_c(*s++);
}


static void rc_dec(__xdata uint16_t v)
{
	static __xdata char b[6];
	static __xdata uint8_t n;

	n = 0;
	do {
		b[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n)
		rc_c(b[--n]);
}


static void rc_dec32(__xdata uint32_t v)
{
	static __xdata char b[10];
	static __xdata uint8_t n;

	n = 0;
	do {
		b[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n)
		rc_c(b[--n]);
}


static void rc_ip(__xdata const uint8_t * __xdata a)
{
	rc_dec(a[0]);
	rc_c('.');
	rc_dec(a[1]);
	rc_c('.');
	rc_dec(a[2]);
	rc_c('.');
	rc_dec(a[3]);
}


static uint8_t ip_is_zero(__xdata const uint8_t * __xdata a)
{
	return !(a[0] | a[1] | a[2] | a[3]);
}


static uint8_t passwd_is_default(void)
{
	static __code const char * __xdata d;
	static __xdata uint8_t k;

	d = DEFAULT_PASSWORD;
	for (k = 0; d[k]; k++) {
		if (passwd[k] != d[k])
			return 0;
	}
	return passwd[k] == 0;
}


static void rc_speed(__xdata uint8_t sp)
{
	switch (sp) {
	case PHY_SPEED_10M:
		rc_s("10");
		break;
	case PHY_SPEED_100M:
		rc_s("100");
		break;
	case PHY_SPEED_1G:
		rc_s("1000");
		break;
	case PHY_SPEED_2G5:
		rc_s("2500");
		break;
	case PHY_SPEED_5G:
		rc_s("5000");
		break;
	case PHY_SPEED_10G:
		rc_s("10000");
		break;
	}
}


static void rc_vlans(void)
{
	static __xdata uint16_t last, next, vid, n;
	static __xdata uint8_t k, any;

	/* ascending VID order, independent of the database slot order */
	any = 0;
	last = 0;
	while (1) {
		next = 0xffff;
		for (k = 0; k < SW_MAX_VLANS; k++) {
			vid = sw_vlans[k];
			if (vid > last && vid < next)
				next = vid;
		}
		if (next == 0xffff)
			break;
		last = next;
		n = vlan_name(next);
		if (next == 1 && n == 0xffff)
			continue;	/* the default VLAN, unnamed */
		rc_s("vlan ");
		rc_dec(next);
		rc_c('\n');
		if (n != 0xffff) {
			rc_s(" name ");
			rc_x((__xdata char *)&vlan_names[n], 1);
			rc_c('\n');
		}
		any = 1;
	}
	if (any)
		rc_s("!\n");
}


static void rc_allowed(__xdata struct sw_port * __xdata sp)
{
	static __xdata uint8_t r;

	if (!sp->nranges) {
		rc_s("none");
		return;
	}
	for (r = 0; r < sp->nranges; r++) {
		if (r)
			rc_c(',');
		rc_dec(sp->allowed[r].lo);
		if (sp->allowed[r].hi != sp->allowed[r].lo) {
			rc_c('-');
			rc_dec(sp->allowed[r].hi);
		}
	}
}


/* Non-default spanning-tree settings of one STP entity (a port or a lag) */
static void rc_stp_ent(__xdata uint8_t e)
{
	static __xdata uint8_t f;

	f = stp_pflags[e];
	if (f & STP_PF_ADMEDGE)
		rc_s(" spanning-tree portfast\n");
	else if (!(f & STP_PF_AUTOEDGE))
		rc_s(" spanning-tree portfast disable\n");
	if (f & STP_PF_BPDUGUARD)
		rc_s(" spanning-tree bpduguard enable\n");
	if (f & STP_PF_FILTER)
		rc_s(" spanning-tree bpdufilter enable\n");
	if (f & STP_PF_ROOTGUARD)
		rc_s(" spanning-tree guard root\n");
	if (stp_pcost[e]) {
		rc_s(" spanning-tree cost ");
		rc_dec32(stp_pcost[e]);
		rc_c('\n');
	}
	if (stp_pprio[e] != 0x80) {
		rc_s(" spanning-tree port-priority ");
		rc_dec(stp_pprio[e]);
		rc_c('\n');
	}
	if (stp_pp2p[e] == 1)
		rc_s(" spanning-tree link-type point-to-point\n");
	else if (stp_pp2p[e] == 2)
		rc_s(" spanning-tree link-type shared\n");
}


static void rc_hash_field(__xdata uint8_t h, __xdata uint8_t bit, __code const char * __xdata w)
{
	if (h & bit) {
		rc_c(' ');
		rc_s(w);
	}
}


/* Port-channels with members come first: a custom hash must be in place
 * before members join, since joining installs the default on a pristine
 * lag */
/* Does any port run LACP in port-channel `group`? */
static uint8_t lacp_in_group(uint8_t group)
{
	static __xdata uint8_t p, gr;

	gr = group;
	for (p = 0; p < LACP_PORTS; p++)
		if (lacp_group[p] == gr)
			return 1;
	return 0;
}


/* The hash all four port-channels share when it is one of the IOS global
 * methods, so it renders as `port-channel load-balance`; 0 otherwise */
static __xdata uint8_t rc_pc_glob;
static __code const uint8_t pclb_bits[] = {
	LAG_HASH_L2_SMAC, LAG_HASH_L2_DMAC, LAG_HASH_L2_SMAC | LAG_HASH_L2_DMAC,
	LAG_HASH_L3_SIP, LAG_HASH_L3_DIP, LAG_HASH_L3_SIP | LAG_HASH_L3_DIP,
	LAG_HASH_L4_SPORT, LAG_HASH_L4_DPORT, LAG_HASH_L4_SPORT | LAG_HASH_L4_DPORT
};
static __code const char * __code const pclb_word[] = {
	"src-mac", "dst-mac", "src-dst-mac", "src-ip", "dst-ip", "src-dst-ip",
	"src-port", "dst-port", "src-dst-port"
};

static void rc_pc_global(void)
{
	static __xdata uint8_t g, h, k;

	rc_pc_glob = 0;
	for (g = 0; g < 4; g++) {
		reg_read_m(RTL837X_TRK_HASH_CTRL_BASE + (g << 2));
		if (!g)
			h = sfr_data[3];
		else if (sfr_data[3] != h)
			return;
	}
	for (k = 0; k < sizeof(pclb_bits); k++) {
		if (pclb_bits[k] == h) {
			rc_pc_glob = h;
			rc_s("port-channel load-balance ");
			rc_s(pclb_word[k]);
			rc_s("\n!\n");
			return;
		}
	}
}


static void rc_port_channels(void)
{
	static __xdata uint8_t g, h;

	for (g = 0; g < 4; g++) {
		if (!port_lag_members_get(g) && !lacp_in_group(g + 1))
			continue;
		rc_s("interface port-channel ");
		rc_dec(g + 1);
		rc_c('\n');
		if (lacp_minlinks[g] != 1) {
			rc_s(" lacp min-links ");
			rc_dec(lacp_minlinks[g]);
			rc_c('\n');
		}
		reg_read_m(RTL837X_TRK_HASH_CTRL_BASE + (g << 2));
		h = sfr_data[3];
		if (h != (rc_pc_glob ? rc_pc_glob : LAG_HASH_DEFAULT)) {
			rc_s(" load-balance");
			rc_hash_field(h, LAG_HASH_SOURCE_PORT_NUMBER, "src-port");
			rc_hash_field(h, LAG_HASH_L2_SMAC, "src-mac");
			rc_hash_field(h, LAG_HASH_L2_DMAC, "dst-mac");
			rc_hash_field(h, LAG_HASH_L3_SIP, "src-ip");
			rc_hash_field(h, LAG_HASH_L3_DIP, "dst-ip");
			rc_hash_field(h, LAG_HASH_L4_SPORT, "l4-src-port");
			rc_hash_field(h, LAG_HASH_L4_DPORT, "l4-dst-port");
			rc_c('\n');
		}
		rc_stp_ent(STP_LAG_BASE + g);
		rc_s("!\n");
	}
}


static void rc_interfaces(void)
{
	static __xdata uint8_t up, lp, g;
	static __xdata uint16_t mtu;
	static __xdata struct sw_port * __xdata sp;

	for (up = 1; up <= 9; up++) {
		lp = machine.phys_to_log_port[up - 1];
		if (lp < machine.min_port || lp > machine.max_port)
			continue;
		sp = &sw_ports[lp];
		rc_s("interface ethernet 1/");
		rc_dec(up);
		rc_c('\n');
		if (port_names[lp][0]) {
			rc_s(" description ");
			rc_x(port_names[lp], 0);
			rc_c('\n');
		}
		if (sp->shut)
			rc_s(" shutdown\n");
		if (sp->speed != PHY_SPEED_AUTO) {
			rc_s(" speed ");
			rc_speed(sp->speed);
			rc_c('\n');
		}
		if (sp->duplex == PHY_DUPLEX_FULL)
			rc_s(" duplex full\n");
		else if (sp->duplex == PHY_DUPLEX_HALF)
			rc_s(" duplex half\n");
		reg_read_m(RTL8373_REG_MAC_L2_PORT_MAX_LEN + ((uint16_t)lp << 8));
		mtu = (((uint16_t)sfr_data[2] << 8) | sfr_data[3]) & 0x3fff;
		if (mtu != 0x3fff) {
			rc_s(" mtu ");
			rc_dec(mtu);
			rc_c('\n');
		}
		if (sp->mode == SW_MODE_TRUNK) {
			rc_s(" switchport mode trunk\n");
			if (sp->native_vid != 1) {
				rc_s(" switchport trunk native vlan ");
				rc_dec(sp->native_vid);
				rc_c('\n');
			}
			if (!(sp->nranges == 1 && sp->allowed[0].lo == 1
			      && sp->allowed[0].hi == SW_VID_MAX)) {
				rc_s(" switchport trunk allowed vlan ");
				rc_allowed(sp);
				rc_c('\n');
			}
		} else if (sp->access_vid != 1) {
			rc_s(" switchport access vlan ");
			rc_dec(sp->access_vid);
			rc_c('\n');
		}
		if (sp->prot)
			rc_s(" switchport protected\n");
		if (sp->eee_off)
			rc_s(" no power efficient-ethernet\n");
		if (sp->rl_in) {
			rc_s(" rate-limit input ");
			rc_dec32(sp->rl_in);
			if (sp->rl_in_drop)
				rc_s(" drop");
			rc_c('\n');
		}
		if (sp->rl_out) {
			rc_s(" rate-limit output ");
			rc_dec32(sp->rl_out);
			rc_c('\n');
		}
		if (igmp_mrouter & ((uint16_t)1 << lp))
			rc_s(" ip igmp snooping mrouter\n");
		if (lacp_fast[lp])
			rc_s(" lacp rate fast\n");
		if (lacp_pprio[lp] != 32768) {
			rc_s(" lacp port-priority ");
			rc_dec(lacp_pprio[lp]);
			rc_c('\n');
		}
		g = port_lag_of(lp);
		if (lacp_group[lp]) {
			rc_s(" channel-group ");
			rc_dec(lacp_group[lp]);
			rc_s(lacp_mode[lp] == LACP_MODE_ACTIVE ? " mode active\n" : " mode passive\n");
		} else if (g != PORT_LAG_NONE) {
			rc_s(" channel-group ");
			rc_dec(g + 1);
			rc_s(" mode on\n");
		} else {
			rc_stp_ent(lp);		/* a member follows its lag */
		}
		rc_s("!\n");
	}
}


/* user-facing port number of a logical port */
static void rc_upnum(__xdata uint8_t lp)
{
	static __xdata uint8_t up;

	for (up = 1; up <= 9; up++) {
		if (machine.phys_to_log_port[up - 1] == lp) {
			rc_dec(up);
			return;
		}
	}
}


static void rc_monitor(void)
{
	static __xdata uint8_t lp;
	static __xdata uint16_t bit;

	for (lp = machine.min_port; lp <= machine.max_port; lp++) {
		bit = (uint16_t)1 << lp;
		if (!((sw_mon_rx | sw_mon_tx) & bit))
			continue;
		rc_s("monitor session 1 source interface ethernet 1/");
		rc_upnum(lp);
		if (!(sw_mon_tx & bit))
			rc_s(" rx");
		else if (!(sw_mon_rx & bit))
			rc_s(" tx");
		rc_c('\n');
	}
	if (sw_mon_dst != SW_MON_NONE) {
		rc_s("monitor session 1 destination interface ethernet 1/");
		rc_upnum(sw_mon_dst);
		rc_c('\n');
	}
}


/* After the interfaces: the per-port edge flags must be in place when
 * `feature spanning-tree` starts the engine */
static void rc_stp_global(void)
{
	if (!stp_rstp)
		rc_s("spanning-tree mode stp\n");
	if (stp_prio != 0x80) {
		rc_s("spanning-tree priority ");
		rc_dec32((uint32_t)stp_prio << 8);
		rc_c('\n');
	}
	if (stp_hello_s != 2) {
		rc_s("spanning-tree hello-time ");
		rc_dec(stp_hello_s);
		rc_c('\n');
	}
	if (stp_fwddelay_s != 15) {
		rc_s("spanning-tree forward-time ");
		rc_dec(stp_fwddelay_s);
		rc_c('\n');
	}
	if (stp_maxage_s != 20) {
		rc_s("spanning-tree max-age ");
		rc_dec(stp_maxage_s);
		rc_c('\n');
	}
	if (stp_txhold != 6) {
		rc_s("spanning-tree transmit hold-count ");
		rc_dec(stp_txhold);
		rc_c('\n');
	}
	if (stp_enabled)
		rc_s("feature spanning-tree\n");
}


static uint8_t rc_mac_changed(void)
{
	static __xdata uint8_t k;

	for (k = 0; k < 6; k++) {
		if (uip_ethaddr.addr[k] != sw_mac_boot[k])
			return 1;
	}
	return 0;
}


static void rc_hex2(__xdata uint8_t b)
{
	rc_c("0123456789abcdef"[b >> 4]);
	rc_c("0123456789abcdef"[b & 0xf]);
}


static void rc_mac(void)
{
	rc_hex2(uip_ethaddr.addr[0]); rc_hex2(uip_ethaddr.addr[1]); rc_c('.');
	rc_hex2(uip_ethaddr.addr[2]); rc_hex2(uip_ethaddr.addr[3]); rc_c('.');
	rc_hex2(uip_ethaddr.addr[4]); rc_hex2(uip_ethaddr.addr[5]);
}


static void rc_emit(void)
{
	rc_s("!\nhostname ");
	rc_x(hostname, 0);
	rc_s("\n!\n");

	rc_vlans();
	if (lacp_sysprio != 32768) {
		rc_s("lacp system-priority ");
		rc_dec(lacp_sysprio);
		rc_s("\n!\n");
	}
	rc_pc_global();
	rc_port_channels();
	rc_interfaces();

	if (management_vlan) {
		rc_s("interface vlan ");
		rc_dec(management_vlan);
		rc_c('\n');
		if (dhcp_state.state != DHCP_OFF) {
			rc_s(" ip address dhcp\n");
		} else {
			rc_s(" ip address ");
			rc_ip((__xdata uint8_t *)uip_hostaddr);
			rc_c(' ');
			rc_ip((__xdata uint8_t *)uip_netmask);
			rc_c('\n');
		}
		if (rc_mac_changed()) {
			rc_s(" mac-address ");
			rc_mac();
			rc_c('\n');
		}
		rc_s("!\n");
	}

	if (dhcp_state.state == DHCP_OFF && !ip_is_zero((__xdata uint8_t *)uip_draddr)) {
		rc_s("ip default-gateway ");
		rc_ip((__xdata uint8_t *)uip_draddr);
		rc_c('\n');
	}
	if (sw_igmp)
		rc_s("ip igmp snooping\n");
	if (syslog_state.enabled) {
		rc_s("logging host ");
		rc_ip(syslog_state.server_ip);
		if (syslog_state.server_port != SYSLOG_PORT_DEFAULT) {
			rc_s(" port ");
			rc_dec(syslog_state.server_port);
		}
		rc_c('\n');
	}
	rc_monitor();
	rc_stp_global();
	if (telnet_state.enabled)
		rc_s("feature telnet\n");

	if (telnet_state.idle_secs != TELNET_IDLE_DEFAULT || !passwd_is_default()) {
		rc_s("!\nline vty\n");
		if (telnet_state.idle_secs == 0xffff) {
			rc_s(" exec-timeout 0 0\n");
		} else if (telnet_state.idle_secs != TELNET_IDLE_DEFAULT) {
			rc_s(" exec-timeout ");
			rc_dec(telnet_state.idle_secs / 60);
			rc_c(' ');
			rc_dec(telnet_state.idle_secs % 60);
			rc_c('\n');
		}
		if (!passwd_is_default()) {
			rc_s(" password ");
			rc_x(passwd, 0);
			rc_c('\n');
		}
	}
	rc_s("!\nend\n");
}


void runcfg_show(void) __banked
{
	rc_tobuf = 0;
	rc_emit();
}


uint16_t runcfg_render(void) __banked
{
	rc_tobuf = 1;
	rc_len = 0;
	rc_over = 0;
	rc_emit();
	rc_tobuf = 0;
	if (rc_over)
		return 0xffff;
	cfg_buf[rc_len] = 0;
	return rc_len;
}


void runcfg_save(void) __banked
{
	static __xdata uint16_t n;

	/* A running TFTP transfer owns cfg_buf and may touch the sector */
	if (tftp_busy()) {
		print_string("% TFTP transfer in progress, try again later\n");
		return;
	}
	print_string("Building configuration...\n");
	n = runcfg_render();
	if (n == 0xffff) {
		print_string("% Configuration exceeds the flash sector, not saved\n");
		return;
	}
	flash_region.addr = CONFIG_START;
	flash_sector_erase();
	flash_region.addr = CONFIG_START;
	flash_region.len = n + 1;
	flash_write_bytes(cfg_buf);
	print_string("[OK]\n");
}


void startup_show(void) __banked
{
	static __xdata uint32_t pos;
	static __xdata uint16_t i;
	static __xdata uint8_t c;

	for (pos = CONFIG_START; pos < CONFIG_START + CONFIG_LEN; pos += FLASH_BUF_SIZE) {
		flash_region.addr = pos;
		flash_region.len = FLASH_BUF_SIZE;
		flash_read_bulk(flash_buf);
		for (i = 0; i < FLASH_BUF_SIZE; i++) {
			c = flash_buf[i];
			if (c == 0 || c == 0xff)
				return;
			write_char(c);
		}
	}
}
