/*
 * show commands. Operational state comes from the drivers (link, counters,
 * lag membership, SFP), configuration from swcfg. Locals live in xdata,
 * see cli.c.
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "rtl837x_stp.h"
#include "rtl837x_phy.h"
#include "machine.h"
#include "dhcp.h"
#include "sfp.h"
#include "uip/uip.h"
#include "swcfg.h"
#include "show.h"
#include "syslog.h"
#include "rtl837x_flash.h"
#include "version.h"

#pragma codeseg BANK3
#pragma constseg BANK3

extern __code const struct machine machine;
extern __xdata uint16_t management_vlan;
extern __xdata uint8_t vlan_names[VLAN_NAMES_SIZE];
extern __xdata struct dhcp_state dhcp_state;
extern __xdata uint8_t sfr_data[4];
extern __xdata uint8_t cmd_history[CMD_HISTORY_SIZE];
extern __xdata uint16_t cmd_history_ptr;

static __xdata uint8_t col;		/* characters printed on the line */

static void sh_c(__xdata char c)
{
	write_char(c);
	if (c == '\n')
		col = 0;
	else
		col++;
}

static void sh_s(__code const char * __xdata s)
{
	while (*s)
		sh_c(*s++);
}

/* xdata string, stopping at NUL or (word) at a space, at most max chars */
static void sh_x(__xdata const char * __xdata s, __xdata uint8_t word, __xdata uint8_t max)
{
	while (*s && max-- && !(word && *s == ' '))
		sh_c(*s++);
}

static void sh_dec(__xdata uint32_t v)
{
	static __xdata char b[10];
	static __xdata uint8_t n;

	n = 0;
	do {
		b[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n)
		sh_c(b[--n]);
}

/* pad with spaces up to column c (at least one space) */
static void sh_to(__xdata uint8_t c)
{
	do {
		sh_c(' ');
	} while (col < c);
}

static void sh_ip(__xdata const uint8_t * __xdata a)
{
	sh_dec(a[0]); sh_c('.');
	sh_dec(a[1]); sh_c('.');
	sh_dec(a[2]); sh_c('.');
	sh_dec(a[3]);
}

/* user-facing port number of a logical port */
static uint8_t sh_up(__xdata uint8_t lp)
{
	static __xdata uint8_t up;

	for (up = 1; up <= 9; up++) {
		if (machine.phys_to_log_port[up - 1] == lp)
			return up;
	}
	return 0;
}

static void sh_ifname(__xdata uint8_t lp)
{
	sh_s("Eth1/");
	sh_dec(sh_up(lp));
}

/* iterate the board's ports in user order: returns the logical port or 0xff */
static __xdata uint8_t it_up;
static uint8_t sh_next_port(void)
{
	static __xdata uint8_t lp;

	while (++it_up <= 9) {
		lp = machine.phys_to_log_port[it_up - 1];
		if (lp >= machine.min_port && lp <= machine.max_port)
			return lp;
	}
	return 0xff;
}
#define FOR_EACH_PORT(lp) for (it_up = 0; ((lp) = sh_next_port()) != 0xff; )

static void sh_speed_code(__xdata uint8_t c)
{
	switch (c) {
	case PORT_LINK_10M:
		sh_s("10");
		break;
	case PORT_LINK_100M:
		sh_s("100");
		break;
	case PORT_LINK_1G:
		sh_s("1000");
		break;
	case PORT_LINK_2G5:
		sh_s("2500");
		break;
	case PORT_LINK_5G:
		sh_s("5000");
		break;
	case PORT_LINK_10G:
		sh_s("10000");
		break;
	default:
		sh_s("auto");
		break;
	}
}

static void sh_cfg_speed(__xdata uint8_t sp)
{
	switch (sp) {
	case PHY_SPEED_10M:
		sh_s("10");
		break;
	case PHY_SPEED_100M:
		sh_s("100");
		break;
	case PHY_SPEED_1G:
		sh_s("1000");
		break;
	case PHY_SPEED_2G5:
		sh_s("2500");
		break;
	case PHY_SPEED_5G:
		sh_s("5000");
		break;
	case PHY_SPEED_10G:
		sh_s("10000");
		break;
	default:
		sh_s("auto");
		break;
	}
}


void show_if_status(void) __banked
{
	static __xdata uint8_t lp, lc, g;
	static __xdata struct sw_port * __xdata sp;

	col = 0;
	sh_s("Port      Name              Status      Vlan     Speed  Type\n"
	     "--------  ----------------  ----------  -------  -----  ------\n");
	FOR_EACH_PORT(lp) {
		sp = &sw_ports[lp];
		sh_ifname(lp);
		sh_to(10);
		sh_x(port_names[lp], 0, 16);
		sh_to(28);
		lc = port_link_code(lp);
		if (sp->shut)
			sh_s("disabled");
		else if (lc == PORT_LINK_DOWN)
			sh_s("notconnect");
		else
			sh_s("connected");
		sh_to(40);
		g = port_lag_of(lp);
		if (g != PORT_LAG_NONE) {
			sh_s("Po");
			sh_dec(g + 1);
		} else if (sp->mode == SW_MODE_TRUNK) {
			sh_s("trunk");
		} else {
			sh_dec(sp->access_vid);
		}
		sh_to(49);
		if (lc != PORT_LINK_DOWN)
			sh_speed_code(lc);
		else
			sh_cfg_speed(sp->speed);
		sh_to(56);
		sh_s(machine.is_sfp[lp] ? "sfp" : "copper");
		sh_c('\n');
	}
}


void show_if_counters(void) __banked
{
	static __xdata uint8_t lp;
	static __xdata uint32_t c[4];

	col = 0;
	sh_s("Port      InPkts       InErrors   OutPkts      OutErrors\n"
	     "--------  -----------  ---------  -----------  ---------\n");
	FOR_EACH_PORT(lp) {
		port_counters_get(lp, c);
		sh_ifname(lp);
		sh_to(10);
		sh_dec(c[2]);
		sh_to(23);
		sh_dec(c[3]);
		sh_to(34);
		sh_dec(c[0]);
		sh_to(47);
		sh_dec(c[1]);
		sh_c('\n');
	}
}


void show_if_trunk(void) __banked
{
	static __xdata uint8_t lp, r, any;
	static __xdata struct sw_port * __xdata sp;

	col = 0;
	any = 0;
	FOR_EACH_PORT(lp) {
		sp = &sw_ports[lp];
		if (sp->mode != SW_MODE_TRUNK)
			continue;
		if (!any)
			sh_s("Port      Native  Allowed VLANs\n"
			     "--------  ------  -------------\n");
		any = 1;
		sh_ifname(lp);
		sh_to(10);
		sh_dec(sp->native_vid);
		sh_to(18);
		if (!sp->nranges)
			sh_s("none");
		for (r = 0; r < sp->nranges; r++) {
			if (r)
				sh_c(',');
			sh_dec(sp->allowed[r].lo);
			if (sp->allowed[r].hi != sp->allowed[r].lo) {
				sh_c('-');
				sh_dec(sp->allowed[r].hi);
			}
		}
		sh_c('\n');
	}
	if (!any)
		sh_s("No trunk ports\n");
}


void show_if_transceiver(void) __banked
{
	static __xdata uint8_t lp, any;

	col = 0;
	any = 0;
	FOR_EACH_PORT(lp) {
		if (!machine.is_sfp[lp])
			continue;
		any = 1;
		sh_ifname(lp);
		sh_s(":\n");
		/* the slot index is is_sfp[] - 1 */
		if (!sfp_print_info(machine.is_sfp[lp] - 1)) {
			sh_s("  no module\n");
			continue;
		}
		sfp_print_measurements(machine.is_sfp[lp] - 1);
	}
	if (!any)
		sh_s("This switch has no SFP ports\n");
}


void show_vlan_brief(void) __banked
{
	static __xdata uint16_t last, next, vid, n;
	static __xdata uint8_t k, lp, first;

	col = 0;
	sh_s("VLAN  Name                              Status  Ports\n"
	     "----  --------------------------------  ------  -------------------------\n");
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
		sh_dec(next);
		sh_to(6);
		n = vlan_name(next);
		if (n != 0xffff) {
			sh_x((__xdata char *)&vlan_names[n], 1, 32);
		} else if (next == 1) {
			sh_s("default");
		} else {
			sh_s("VLAN");
			if (next < 1000) sh_c('0');
			if (next < 100) sh_c('0');
			if (next < 10) sh_c('0');
			sh_dec(next);
		}
		sh_to(40);
		sh_s("active");
		sh_to(48);
		/* access ports, as the industry-standard listing does; trunks
		 * are in show interfaces trunk */
		first = 1;
		FOR_EACH_PORT(lp) {
			if (sw_ports[lp].mode != SW_MODE_ACCESS || sw_ports[lp].access_vid != next)
				continue;
			if (!first) {
				sh_c(',');
				if (col > 70) {
					sh_c('\n');
					sh_to(47);
				}
				sh_c(' ');
			}
			sh_ifname(lp);
			first = 0;
		}
		sh_c('\n');
	}
}


void show_po_summary(void) __banked
{
	static __xdata uint8_t g, lp, h, any;
	static __xdata uint16_t m;

	col = 0;
	any = 0;
	for (g = 0; g < 4; g++) {
		m = port_lag_members_get(g);
		if (!m)
			continue;
		if (!any)
			sh_s("Port-channel  Members                     Hash\n"
			     "------------  --------------------------  ---------------------\n");
		any = 1;
		sh_s("Po");
		sh_dec(g + 1);
		sh_to(14);
		FOR_EACH_PORT(lp) {
			if (m & ((uint16_t)1 << lp)) {
				sh_ifname(lp);
				if (port_link_code(lp) == PORT_LINK_DOWN)
					sh_s("(D)");
				sh_c(' ');
			}
		}
		sh_to(42);
		reg_read_m(RTL837X_TRK_HASH_CTRL_BASE + (g << 2));
		h = sfr_data[3];
		if (h & LAG_HASH_SOURCE_PORT_NUMBER) sh_s("src-port ");
		if (h & LAG_HASH_L2_SMAC) sh_s("src-mac ");
		if (h & LAG_HASH_L2_DMAC) sh_s("dst-mac ");
		if (h & LAG_HASH_L3_SIP) sh_s("src-ip ");
		if (h & LAG_HASH_L3_DIP) sh_s("dst-ip ");
		if (h & LAG_HASH_L4_SPORT) sh_s("l4-src-port ");
		if (h & LAG_HASH_L4_DPORT) sh_s("l4-dst-port");
		sh_c('\n');
	}
	if (!any)
		sh_s("No port-channels\n");
	else
		sh_s("(D) = member link down\n");
}


void show_ip_if_brief(void) __banked
{
	col = 0;
	sh_s("Interface  IP-Address       Netmask          Method\n"
	     "---------  ---------------  ---------------  ------\n");
	sh_s("Vlan");
	sh_dec(management_vlan);
	sh_to(11);
	sh_ip((__xdata uint8_t *)uip_hostaddr);
	sh_to(28);
	sh_ip((__xdata uint8_t *)uip_netmask);
	sh_to(45);
	sh_s(dhcp_state.state != DHCP_OFF ? "dhcp" : "static");
	sh_s("\nDefault gateway: ");
	sh_ip((__xdata uint8_t *)uip_draddr);
	sh_c('\n');
}


void show_monitor(void) __banked
{
	static __xdata uint8_t lp;
	static __xdata uint16_t bit;

	col = 0;
	if (sw_mon_dst == SW_MON_NONE && !(sw_mon_rx | sw_mon_tx)) {
		sh_s("Session 1 is not configured\n");
		return;
	}
	sh_s("Session 1 (");
	sh_s(sw_mon_dst != SW_MON_NONE && (sw_mon_rx | sw_mon_tx) ? "active" : "incomplete");
	sh_s(")\n  Source rx:    ");
	FOR_EACH_PORT(lp) {
		bit = (uint16_t)1 << lp;
		if (sw_mon_rx & bit) {
			sh_ifname(lp);
			sh_c(' ');
		}
	}
	sh_s("\n  Source tx:    ");
	FOR_EACH_PORT(lp) {
		bit = (uint16_t)1 << lp;
		if (sw_mon_tx & bit) {
			sh_ifname(lp);
			sh_c(' ');
		}
	}
	sh_s("\n  Destination:  ");
	if (sw_mon_dst != SW_MON_NONE)
		sh_ifname(sw_mon_dst);
	sh_c('\n');
}


void show_mac_table(void) __banked
{
	port_l2_learned();
}


void show_stp(void) __banked
{
	stp_status();
}


void show_version(void) __banked
{
	static __xdata uint32_t up;
	static __xdata uint8_t k;

	col = 0;
	sh_s("rtl-swos " VERSION_SW "\nBuilt:     " BUILD_DATE "\nHardware:  ");
	print_string(machine.machine_name);
	sh_s("\nFlash:     ");
	print_string(get_flash_size_str());
	sh_s("\nMAC:       ");
	for (k = 0; k < 6; k++) {
		if (k)
			sh_c(':');
		print_byte(uip_ethaddr.addr[k]);
	}
	reg_read_m(RTL837X_REG_SEC_COUNTER);
	up = ((uint32_t)sfr_data[0] << 24) | ((uint32_t)sfr_data[1] << 16)
	     | ((uint16_t)sfr_data[2] << 8) | sfr_data[3];
	sh_s("\nUptime:    ");
	if (up >= 86400UL) {
		sh_dec(up / 86400UL);
		sh_s("d ");
	}
	sh_dec((up / 3600) % 24);
	sh_s("h ");
	sh_dec((up / 60) % 60);
	sh_s("m ");
	sh_dec(up % 60);
	sh_s("s\n");
}


/* The console's recall history, oldest first */
void show_history(void) __banked
{
	static __xdata uint16_t p;
	static __xdata uint8_t begun, c;

	p = (cmd_history_ptr + 1) & CMD_HISTORY_MASK;
	begun = 0;
	while (p != cmd_history_ptr) {
		c = cmd_history[p];
		if (!c || c == '\n')
			begun = 1;
		if (begun && c)
			write_char(c);
		p = (p + 1) & CMD_HISTORY_MASK;
	}
}


void show_logging(void) __banked
{
	col = 0;
	if (!syslog_state.enabled) {
		sh_s("Remote syslog: off\n");
		return;
	}
	sh_s("Remote syslog: ");
	sh_ip(syslog_state.server_ip);
	sh_c(':');
	sh_dec(syslog_state.server_port);
	sh_c('\n');
}
