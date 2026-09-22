/*
 * Actions of the modal CLI commands. See cli_act.h.
 *
 * Kept in BANK3, apart from the engine and the command tree in BANK1,
 * which were running out of room. Locals live in xdata: the 8051
 * overlay segment is exhausted, see cli.c.
 */
#include "rtl837x_common.h"
#include "console.h"
#include "rtl837x_phy.h"
#include "phy.h"
#include "machine.h"
#include "rtl837x_igmp.h"
#include "boot.h"
#include "swcfg.h"
#include "runcfg.h"
#include "telnetd.h"
#include "rtl837x_stp.h"
#include "rtl837x_port.h"
#include "rtl837x_regs.h"
#include "cli.h"
#include "cli_act.h"
#include "show.h"
#include "tftp.h"
#include "dbgcmd.h"
#include "sfp.h"

#pragma codeseg BANK3
#pragma constseg BANK3

extern __xdata char hostname[24];
extern __xdata struct phy_settings phy_settings;
extern __code const struct machine machine;
extern __xdata uint16_t management_vlan;
extern __xdata char passwd[21];
void reset_chip(void);


/* ---------------- config handler helpers ---------------- */

static __xdata uint8_t d_rc, d_lp;
static __xdata uint16_t d_v;

/* A name token: letters, digits, '-', '_', '.'; at least one char */
static uint8_t name_ok(__xdata const char * __xdata s)
{
	static __xdata const char * __xdata p;
	static __xdata char c;

	p = s;
	if (!*p || *p == ' ')
		return 0;
	while ((c = *p) && c != ' ') {
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
		      || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
			return 0;
		p++;
	}
	return 1;
}


static void sw_err(__xdata uint8_t rc)
{
	switch (rc) {
	case SW_ERR_FULL:
		print_string("% VLAN database full\n");
		break;
	case SW_ERR_RANGE:
		print_string("% VLAN id out of range (1-4094)\n");
		break;
	case SW_ERR_VLAN1:
		print_string("% Default VLAN 1 may not be deleted\n");
		break;
	case SW_ERR_SYNTAX:
		print_string("% Invalid VLAN list\n");
		break;
	}
}


/* Make sure a VLAN referenced by a port or the management interface
 * exists. Returns 0 when the database is full, 1 when the VLAN existed,
 * 2 when it was just created (the caller must sw_apply()). */
static uint8_t vlan_ensure(__xdata uint16_t vid)
{
	if (sw_vlan_exists(vid))
		return 1;
	if (sw_vlan_add(vid) != SW_OK) {
		print_string("% VLAN database full\n");
		return 0;
	}
	print_string("% VLAN ");
	itoa_short(vid);
	print_string(" did not exist, created it\n");
	return 2;
}


/* User-facing port N -> logical port through the board table, exactly
 * like the legacy `port N` command; 0xff when the board has no such port */
static uint8_t up_to_lp(__xdata uint16_t up)
{
	static __xdata uint8_t lp;

	if (up < 1 || up > 9)
		return 0xff;
	lp = machine.phys_to_log_port[up - 1];
	if (lp < machine.min_port || lp > machine.max_port)
		return 0xff;
	return lp;
}


static void bad_value(void)
{
	print_string("% Value out of range\n");
}


/* The STP entity the current interface-mode context configures, or
 * 0xff (with a message) for a port that belongs to a port-channel */
static uint8_t stp_ctx_entity(void)
{
	static __xdata uint8_t e;

	if (cli.mode == CLI_MODE_PO)
		return STP_LAG_BASE + cli.ctx_po - 1;
	e = stp_cfg_entity(cli.ctx_lport);
	if (e != cli.ctx_lport) {
		print_string("% Port is in a port-channel: configure spanning-tree"
			     " under interface port-channel ");
		itoa_short(e - STP_LAG_BASE + 1);
		write_char('\n');
		return 0xff;
	}
	return e;
}


void cli_act(uint8_t action) __banked
{
	switch (action) {
	case ACT_ENABLE:
		if (cli.mode == CLI_MODE_EXEC)
			cli.mode = CLI_MODE_PRIV;
		break;
	case ACT_DISABLE:
		cli.mode = CLI_MODE_EXEC;
		break;
	case ACT_CONF_T:
		cli.mode = CLI_MODE_CONFIG;
		print_string("Enter configuration commands, one per line. End with 'end'.\n");
		break;
	case ACT_EXIT:
		switch (cli.mode) {
		case CLI_MODE_CONFIG:
			cli.mode = CLI_MODE_PRIV;
			break;
		case CLI_MODE_IF:
		case CLI_MODE_VLAN:
		case CLI_MODE_LINE:
		case CLI_MODE_SVI:
		case CLI_MODE_PO:
			cli.mode = CLI_MODE_CONFIG;
			break;
		/* EXEC/PRIV: telnet intercepts `exit` itself; nothing to do
		 * on the serial console. */
		}
		break;
	case ACT_END:
		if (cli.mode >= CLI_MODE_CONFIG)
			cli.mode = CLI_MODE_PRIV;
		break;
	case ACT_WRITE:
		runcfg_save();
		break;
	case ACT_SHOW_RUN:
		runcfg_show();
		break;
	case ACT_SHOW:
		switch (cli.lo) {
		case SHOW_IF_STATUS:
			show_if_status();
			break;
		case SHOW_IF_COUNT:
			show_if_counters();
			break;
		case SHOW_IF_TRUNK:
			show_if_trunk();
			break;
		case SHOW_IF_XCVR:
			show_if_transceiver();
			break;
		case SHOW_VLAN:
			show_vlan_brief();
			break;
		case SHOW_PO:
			show_po_summary();
			break;
		case SHOW_IP_IF:
			show_ip_if_brief();
			break;
		case SHOW_MON:
			if (cli.nargs && cli.args[0] != 1) {
				print_string("% There is only session 1\n");
				break;
			}
			show_monitor();
			break;
		case SHOW_MAC:
			show_mac_table();
			break;
		case SHOW_STP:
			show_stp();
			break;
		case SHOW_TFTP:
			tftp_show();
			break;
		case SHOW_VER:
			show_version();
			break;
		case SHOW_HIST:
			show_history();
			break;
		case SHOW_LOG:
			show_logging();
			break;
		case SHOW_IGMP:
			igmp_show();
			break;
		}
		break;
	case ACT_CLEAR_MAC:
		port_l2_forget();
		break;
	case ACT_DEBUG:
		debug_run(cli.lo);
		break;
	case ACT_COPY:
	{
		static __xdata uint8_t srv[4];
		srv[0] = cli.args[0] >> 24;
		srv[1] = cli.args[0] >> 16;
		srv[2] = cli.args[0] >> 8;
		srv[3] = cli.args[0];
		tftp_begin(cli.lo, srv, cli.line + cli.argoff[1]);
		break;
	}
	case ACT_MACADDR:
	{
		static __xdata uint8_t mac[6];
		if (cli.no) {
			sw_mgmt_mac_set(sw_mac_boot);
			break;
		}
		if (!sw_mac_parse(cli.line + cli.argoff[0], mac)) {
			print_string("% Invalid MAC address\n");
			break;
		}
		if (!sw_mgmt_mac_set(mac))
			print_string("% The MAC must be unicast and globally administered\n");
		break;
	}
	case ACT_SHOW_START:
		startup_show();
		break;
	case ACT_RELOAD:
		print_string("\nRELOAD\n\n");
		reset_chip();
		break;
	case ACT_IF:
		/* User-facing port N maps through the board table, exactly
		 * like the legacy `port N` command: on 4+2 boards the
		 * logical numbering does not start at 0. */
		d_v = cli.args[0];
		d_lp = up_to_lp(d_v);
		if (d_lp == 0xff) {
			print_string("% Invalid interface\n");
			break;
		}
		cli.ctx_if = d_v;
		cli.ctx_lport = d_lp;
		cli.mode = CLI_MODE_IF;
		break;
	case ACT_SVI:
		cli.ctx_vlan = cli.args[0];
		cli.mode = CLI_MODE_SVI;
		break;
	case ACT_VLAN:
		d_v = cli.args[0];
		if (cli.no) {
			d_rc = sw_vlan_del(d_v);
			if (d_rc)
				sw_err(d_rc);
			else if (d_v == management_vlan)
				print_string("% Warning: that was the management VLAN\n");
			break;
		}
		if (!sw_vlan_exists(d_v)) {
			d_rc = sw_vlan_add(d_v);
			if (d_rc) {
				sw_err(d_rc);
				break;
			}
			sw_apply();
		}
		cli.ctx_vlan = d_v;
		cli.mode = CLI_MODE_VLAN;
		break;
	case ACT_VLAN_NAME:
		if (cli.no) {
			sw_vlan_name_set(cli.ctx_vlan, 0);
			break;
		}
		if (!name_ok(cli.line + cli.argoff[0])) {
			print_string("% Invalid name\n");
			break;
		}
		d_rc = sw_vlan_name_set(cli.ctx_vlan, cli.line + cli.argoff[0]);
		if (d_rc)
			print_string("% VLAN name table full\n");
		break;
	case ACT_SHUT:
		if (machine.is_sfp[cli.ctx_lport]) {
			print_string("% shutdown is not supported on SFP ports\n");
			break;
		}
		/* no shutdown brings the port back at its configured speed */
		sw_ports[cli.ctx_lport].shut = !cli.no;
		phy_settings.port = cli.ctx_lport;
		phy_settings.duplex = sw_ports[cli.ctx_lport].duplex;
		phy_settings.speed = cli.no ? sw_ports[cli.ctx_lport].speed : PHY_OFF;
		phy_set_speed();
		break;
	case ACT_SPEED:
		d_v = cli.no ? PHY_SPEED_AUTO : cli.lo;
		if (machine.is_sfp[cli.ctx_lport]) {
			/* an SFP port's speed selects the SerDes mode */
			static __xdata uint8_t slot, sfs;
			switch (d_v) {
			case PHY_SPEED_AUTO: sfs = SFP_SPEED_AUTO; break;
			case PHY_SPEED_100M: sfs = SFP_SPEED_100M; break;
			case PHY_SPEED_1G: sfs = SFP_SPEED_1G; break;
			case PHY_SPEED_2G5: sfs = SFP_SPEED_2G5; break;
			case PHY_SPEED_10G: sfs = SFP_SPEED_10G; break;
			default:
				print_string("% SFP ports support 100, 1000, 2500, 10000 and auto\n");
				sfs = 0xff;
			}
			if (sfs == 0xff)
				break;
			sw_ports[cli.ctx_lport].speed = d_v;
			slot = machine.is_sfp[cli.ctx_lport] - 1;
			sfp_speed[slot] = sfs;
			sfp_pins_last |= 0x1 << (slot << 2);	/* re-run module setup */
			handle_sfp();
			break;
		}
		/* a shut port keeps the speed for its no shutdown */
		sw_ports[cli.ctx_lport].speed = d_v;
		if (sw_ports[cli.ctx_lport].shut)
			break;
		phy_settings.port = cli.ctx_lport;
		phy_settings.duplex = sw_ports[cli.ctx_lport].duplex;
		phy_settings.speed = d_v;
		phy_set_speed();
		break;
	case ACT_DUPLEX:
		if (machine.is_sfp[cli.ctx_lport]) {
			print_string("% SFP ports are full duplex\n");
			break;
		}
		sw_ports[cli.ctx_lport].duplex = cli.no ? PHY_DUPLEX_BOTH : cli.lo;
		if (sw_ports[cli.ctx_lport].shut)
			break;
		phy_settings.port = cli.ctx_lport;
		phy_settings.duplex = sw_ports[cli.ctx_lport].duplex;
		phy_settings.speed = sw_ports[cli.ctx_lport].speed;
		phy_set_speed();
		break;
	case ACT_DESC:
	{
		static __xdata char * __xdata dd;
		static __xdata char * __xdata ds;
		static __xdata uint8_t dn;
		dd = port_names[cli.ctx_lport];
		if (cli.no || !cli.nargs) {
			*dd = 0;
			break;
		}
		ds = cli.line + cli.argoff[0];
		for (dn = 0; *ds && dn < PORT_NAME_SIZE - 1; dn++)
			*dd++ = *ds++;
		*dd = 0;
		break;
	}
	case ACT_MTU:
		sw_mtu_set(cli.ctx_lport, cli.no ? 16383 : cli.args[0]);
		break;
	case ACT_SW_MODE:
		sw_ports[cli.ctx_lport].mode = cli.no ? SW_MODE_ACCESS : cli.lo;
		sw_apply();
		break;
	case ACT_SW_ACCESS:
		d_v = cli.no ? 1 : cli.args[0];
		if (!vlan_ensure(d_v))
			break;
		sw_ports[cli.ctx_lport].access_vid = d_v;
		sw_apply();
		break;
	case ACT_SW_NATIVE:
		sw_ports[cli.ctx_lport].native_vid = cli.no ? 1 : cli.args[0];
		sw_apply();
		break;
	case ACT_SW_ALLOWED:
		d_rc = cli.no ? SW_AL_ALL : cli.lo;
		d_rc = sw_allowed_edit(cli.ctx_lport, d_rc,
				       cli.nargs ? cli.line + cli.argoff[0] : 0);
		if (d_rc == SW_ERR_FULL) {
			print_string("% Too many VLAN ranges (max 8)\n");
			break;
		}
		if (d_rc) {
			sw_err(d_rc);
			break;
		}
		sw_apply();
		break;
	case ACT_HOSTNAME:
	{
		static __xdata char * __xdata hs;
		static __xdata uint8_t hn;
		if (cli.no) {
			hostname[0] = 0;
			set_hostname_default();
			break;
		}
		hs = cli.line + cli.argoff[0];
		if (!name_ok(hs)) {
			print_string("% Invalid hostname\n");
			break;
		}
		for (hn = 0; hn < sizeof(hostname) - 1 && hs[hn] && hs[hn] != ' '; hn++)
			hostname[hn] = hs[hn];
		hostname[hn] = 0;
		break;
	}
	case ACT_IP_ADDR:
		if (cli.no) {
			sw_mgmt_ip_set(0, 0);
			break;
		}
		d_rc = vlan_ensure(cli.ctx_vlan);
		if (!d_rc)
			break;
		if (d_rc == 2)
			sw_apply();
		sw_mgmt_vlan_set(cli.ctx_vlan);
		sw_mgmt_ip_set(cli.args[0], cli.args[1]);
		break;
	case ACT_IP_DHCP:
		d_rc = vlan_ensure(cli.ctx_vlan);
		if (!d_rc)
			break;
		if (d_rc == 2)
			sw_apply();
		sw_mgmt_vlan_set(cli.ctx_vlan);
		sw_mgmt_dhcp();
		break;
	case ACT_DEFGW:
		sw_gateway_set(cli.no ? 0 : cli.args[0]);
		break;
	case ACT_IGMP:
		sw_igmp = !cli.no;
		if (cli.no)
			igmp_setup();
		else
			igmp_enable();
		break;
	case ACT_LOG_HOST:
		if (cli.no)
			sw_logging_off();
		else
			sw_logging_host(cli.args[0], cli.nargs >= 2 ? cli.args[1] : 0);
		break;
	case ACT_FEAT_TELNET:
		if (cli.no)
			telnet_stop();
		else
			telnet_start();
		break;
	case ACT_LINE_VTY:
		cli.ctx_line = 1;
		cli.mode = CLI_MODE_LINE;
		break;
	case ACT_EXEC_TO:
		if (cli.no) {
			d_v = TELNET_IDLE_DEFAULT;
		} else {
			d_v = cli.args[0] * 60 + (cli.nargs >= 2 ? cli.args[1] : 0);
			if (!d_v)
				d_v = 0xffff;	/* 0 0: effectively never */
			else if (d_v < 30) {
				print_string("% Minimum timeout is 30 seconds\n");
				break;
			}
		}
		telnet_set_timeout(d_v);
		break;
	case ACT_VTY_PW:
	{
		static __xdata char * __xdata ps;
		static __xdata uint8_t pn;
		if (cli.no) {
			strtox((__xdata uint8_t *)passwd, DEFAULT_PASSWORD);
			break;
		}
		ps = cli.line + cli.argoff[0];
		for (pn = 0; pn < sizeof(passwd) - 1 && ps[pn] && ps[pn] != ' '; pn++)
			passwd[pn] = ps[pn];
		passwd[pn] = 0;
		break;
	}
	case ACT_EEE:
		sw_ports[cli.ctx_lport].eee_off = cli.no;
		sw_eee_apply(cli.ctx_lport);
		break;
	case ACT_PROT:
		sw_ports[cli.ctx_lport].prot = !cli.no;
		sw_protect_apply();
		break;
	case ACT_RL:
	{
		static __xdata uint8_t dir;
		static __xdata uint32_t kb;
		dir = cli.lo;
		if (cli.no) {
			kb = 0;
		} else {
			kb = cli.args[0] & ~15UL;	/* the hardware steps in 16 kbit/s */
			if (cli.args[0] < SW_RATE_MIN || cli.args[0] > SW_RATE_MAX) {
				bad_value();
				break;
			}
		}
		if (dir == 2) {
			sw_ports[cli.ctx_lport].rl_out = kb;
		} else {
			sw_ports[cli.ctx_lport].rl_in = kb;
			sw_ports[cli.ctx_lport].rl_in_drop = (dir == 3);
		}
		sw_rate_apply(cli.ctx_lport);
		break;
	}
	case ACT_CHGRP:
		sw_lag_join(cli.ctx_lport, cli.no ? 0 : cli.args[0]);
		sw_apply();	/* members share one PVID */
		break;
	case ACT_PO:
		cli.ctx_po = cli.args[0];
		cli.mode = CLI_MODE_PO;
		break;
	case ACT_LB:
		if (cli.no) {
			port_lag_hash_set(cli.ctx_po - 1, LAG_HASH_DEFAULT);
			break;
		}
		port_lag_hash_set(cli.ctx_po - 1, cli.acc);
		break;
	case ACT_MON_SRC:
	{
		static __xdata uint16_t bit;
		d_lp = up_to_lp(cli.args[1]);
		if (d_lp == 0xff) {
			print_string("% Invalid interface\n");
			break;
		}
		bit = (uint16_t)1 << d_lp;
		sw_mon_rx &= ~bit;
		sw_mon_tx &= ~bit;
		if (!cli.no) {
			if (d_lp == sw_mon_dst) {
				print_string("% The destination cannot be a source\n");
				break;
			}
			if (cli.lo & 1)
				sw_mon_rx |= bit;
			if (cli.lo & 2)
				sw_mon_tx |= bit;
		}
		sw_mon_apply();
		break;
	}
	case ACT_MON_DST:
		if (cli.no) {
			sw_mon_dst = SW_MON_NONE;
		} else {
			d_lp = up_to_lp(cli.args[1]);
			if (d_lp == 0xff) {
				print_string("% Invalid interface\n");
				break;
			}
			sw_mon_dst = d_lp;
			sw_mon_rx &= ~((uint16_t)1 << d_lp);
			sw_mon_tx &= ~((uint16_t)1 << d_lp);
		}
		sw_mon_apply();
		break;
	case ACT_MON_DEL:
		sw_mon_dst = SW_MON_NONE;
		sw_mon_rx = 0;
		sw_mon_tx = 0;
		sw_mon_apply();
		break;
	case ACT_FEAT_STP:
		stp_cfg_enable(!cli.no);
		break;
	case ACT_STP_G:
	{
		static __xdata uint32_t sv;
		sv = cli.args[0];
		switch (cli.lo) {
		case STPG_RSTP:
			stp_rstp = 1;	/* also `no spanning-tree mode` */
			break;
		case STPG_STP:
			stp_rstp = cli.no;
			break;
		case STPG_PRIO:
			if (cli.no)
				sv = 32768;
			if (sv > 61440 || (sv & 4095)) {
				bad_value();
				break;
			}
			stp_cfg_prio(sv >> 8);
			break;
		case STPG_HELLO:
			if (cli.no)
				sv = 2;
			if (sv < 1 || sv > 10) {
				bad_value();
				break;
			}
			stp_hello_s = sv;
			break;
		case STPG_FWD:
			if (cli.no)
				sv = 15;
			if (sv < 4 || sv > 30) {
				bad_value();
				break;
			}
			stp_fwddelay_s = sv;
			break;
		case STPG_MAXAGE:
			if (cli.no)
				sv = 20;
			if (sv < 6 || sv > 40) {
				bad_value();
				break;
			}
			stp_maxage_s = sv;
			break;
		case STPG_TXHOLD:
			if (cli.no)
				sv = 6;
			if (sv < 1 || sv > 10) {
				bad_value();
				break;
			}
			stp_txhold = sv;
			break;
		}
		break;
	}
	case ACT_STP_IF:
	{
		static __xdata uint8_t e;
		static __xdata uint32_t iv;
		e = stp_ctx_entity();
		if (e == 0xff)
			break;
		iv = cli.args[0];
		switch (cli.lo) {
		case STPI_PORTFAST:
			stp_pflags[e] &= ~(STP_PF_ADMEDGE | STP_PF_AUTOEDGE | STP_PF_OPEREDGE);
			if (cli.no)
				stp_pflags[e] |= STP_PF_AUTOEDGE;	/* the default */
			else
				stp_pflags[e] |= STP_PF_ADMEDGE | STP_PF_OPEREDGE;
			break;
		case STPI_PF_DIS:
			stp_pflags[e] &= ~(STP_PF_ADMEDGE | STP_PF_AUTOEDGE | STP_PF_OPEREDGE);
			break;
		case STPI_BPDUGUARD:
			if (cli.no)
				stp_pflags[e] &= ~STP_PF_BPDUGUARD;
			else
				stp_pflags[e] |= STP_PF_BPDUGUARD;
			break;
		case STPI_BPDUFILT:
			if (cli.no)
				stp_pflags[e] &= ~STP_PF_FILTER;
			else
				stp_pflags[e] |= STP_PF_FILTER;
			break;
		case STPI_ROOTGUARD:
			if (cli.no)
				stp_pflags[e] &= ~STP_PF_ROOTGUARD;
			else
				stp_pflags[e] |= STP_PF_ROOTGUARD;
			break;
		case STPI_COST:
			if (cli.no)
				iv = 0;
			else if (iv < 1 || iv > 200000000UL) {
				bad_value();
				break;
			}
			stp_pcost[e] = iv;
			break;
		case STPI_PPRIO:
			if (cli.no)
				iv = 128;
			if (iv > 240 || (iv & 15)) {
				bad_value();
				break;
			}
			stp_pprio[e] = iv;
			break;
		case STPI_P2P:
			stp_pp2p[e] = cli.no ? 0 : 1;
			break;
		case STPI_SHARED:
			stp_pp2p[e] = 2;
			break;
		}
		break;
	}
	}
}
