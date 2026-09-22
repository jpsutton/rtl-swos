/*
 * Modal CLI engine. See cli.h for the model.
 *
 * The command tree lives in code space; matching walks one token at a
 * time with unique-prefix abbreviation. Literal tokens win over
 * argument placeholders. EXEC commands are reachable from config modes
 * without `do` (mode root first, EXEC root second), and a line whose
 * first token no tree claims falls back to the legacy flat parser so
 * unported commands keep working during the migration.
 *
 * There is ONE cli state shared by the serial console and telnet: on
 * this hardware the sessions share the underlying command machinery,
 * so a mode change on one is visible on the other. Documented
 * limitation until the legacy parser is gone.
 *
 * Handlers are action ids dispatched from a switch, not function
 * pointers: banked function pointers are fragile with SDCC, ids are
 * free.
 */
#include "rtl837x_common.h"
#include "cmd_parser.h"
#include "rtl837x_phy.h"
#include "phy.h"
#include "machine.h"
#include "rtl837x_igmp.h"
#include "boot.h"
#include "swcfg.h"
#include "cli.h"

#pragma codeseg BANK1
#pragma constseg BANK1

extern __xdata char hostname[24];
extern __xdata struct phy_settings phy_settings;
extern __code const struct machine machine;
extern __xdata uint16_t management_vlan;
void reset_chip(void);

__xdata struct cli_state_t cli;
/* Printable prompt width (without the leading newline), for aligning
 * the '^' error marker under the echoed line. */
__xdata uint8_t cli_plen;

/* ---- tokenizer state ---- */
#define CLI_MAX_TOKS 10
static __xdata uint8_t tok_off[CLI_MAX_TOKS];
static __xdata uint8_t tok_len[CLI_MAX_TOKS];
static __xdata uint8_t ntok;
static __xdata uint8_t trailing_space;
static __xdata char *cli_line;

/* ---- walk result ---- */
static __xdata uint8_t w_status;
#define W_OK		0	/* stopped at w_node with all tokens eaten */
#define W_NOMATCH0	1	/* first token matched nothing */
#define W_INVALID	2	/* some later token matched nothing */
#define W_AMBIG		3
static __xdata uint8_t w_badtok;	/* token index of the failure */
static __code const struct cli_node * __xdata w_node;
static __code const struct cli_node * __code const * __xdata w_children;

/* ---------------- actions ---------------- */
#define ACT_NONE	0
#define ACT_ENABLE	1
#define ACT_DISABLE	2
#define ACT_CONF_T	3
#define ACT_EXIT	4
#define ACT_END		5
#define ACT_SHOW_VER	6
#define ACT_WRITE	7
#define ACT_RELOAD	8
#define ACT_IF		9
#define ACT_VLAN	10
#define ACT_LEGACY	11	/* re-run the whole line in the legacy parser */
#define ACT_SHUT	12	/* interface: shutdown / no shutdown */
#define ACT_SPEED	13	/* interface: speed <val> (value in node->lo) */
#define ACT_DESC	14	/* interface: description LINE / no description */
#define ACT_SVI		15	/* interface vlan N */
#define ACT_VLAN_NAME	16
#define ACT_SW_MODE	17	/* mode in node->lo */
#define ACT_SW_ACCESS	18
#define ACT_SW_NATIVE	19
#define ACT_SW_ALLOWED	20	/* operation in node->lo */
#define ACT_MTU		21
#define ACT_HOSTNAME	22
#define ACT_IP_ADDR	23
#define ACT_IP_DHCP	24
#define ACT_DEFGW	25
#define ACT_IGMP	26
#define ACT_LOG_HOST	27

/* ---------------- command tree ---------------- */

/* leaf with no children */
#define NO_CHILDREN 0

static __code const struct cli_node n_conf_terminal = {
	"terminal", 0, CLI_F_PRIV, 0, 0, NO_CHILDREN, ACT_CONF_T,
	"Configure from the terminal"
};
static __code const struct cli_node * __code const ch_configure[] = {
	&n_conf_terminal, 0
};

static __code const struct cli_node n_show_version = {
	"version", 0, 0, 0, 0, NO_CHILDREN, ACT_SHOW_VER,
	"System software and hardware status"
};
static __code const struct cli_node * __code const ch_show[] = {
	&n_show_version, 0
};

static __code const struct cli_node n_write_memory = {
	"memory", 0, CLI_F_PRIV, 0, 0, NO_CHILDREN, ACT_WRITE,
	"Write the running configuration to flash"
};
static __code const struct cli_node * __code const ch_write[] = {
	&n_write_memory, 0
};

/* copy running-config startup-config, plus the legacy TFTP forms
 * (copy tftp ... / copy config tftp ...) passed through verbatim */
static __code const struct cli_node n_copy_run_start = {
	"startup-config", 0, CLI_F_PRIV, 0, 0, NO_CHILDREN, ACT_WRITE,
	"Save to the startup configuration"
};
static __code const struct cli_node * __code const ch_copy_run[] = {
	&n_copy_run_start, 0
};
static __code const struct cli_node n_copy_running = {
	"running-config", 0, CLI_F_PRIV, 0, 0, ch_copy_run, ACT_NONE,
	"Copy from the running configuration"
};
static __code const struct cli_node n_arg_rest_legacy = {
	0, CLI_A_LINE, CLI_F_PRIV, 0, 0, NO_CHILDREN, ACT_LEGACY,
	"flash|config <server-ip> <filename>"
};
static __code const struct cli_node * __code const ch_copy_tftp[] = {
	&n_arg_rest_legacy, 0
};
static __code const struct cli_node n_copy_tftp = {
	"tftp", 0, CLI_F_PRIV, 0, 0, ch_copy_tftp, ACT_NONE,
	"Download from a TFTP server"
};
static __code const struct cli_node n_copy_config = {
	"config", 0, CLI_F_PRIV, 0, 0, ch_copy_tftp, ACT_NONE,
	"Upload the startup config (copy config tftp <ip> <file>)"
};
static __code const struct cli_node * __code const ch_copy[] = {
	&n_copy_running, &n_copy_tftp, &n_copy_config, 0
};

static __code const struct cli_node n_enable = {
	"enable", 0, 0, 0, 0, NO_CHILDREN, ACT_ENABLE,
	"Turn on privileged commands"
};
static __code const struct cli_node n_disable = {
	"disable", 0, CLI_F_PRIV, 0, 0, NO_CHILDREN, ACT_DISABLE,
	"Turn off privileged commands"
};
static __code const struct cli_node n_configure = {
	"configure", 0, CLI_F_PRIV, 0, 0, ch_configure, ACT_NONE,
	"Enter configuration mode"
};
static __code const struct cli_node n_show = {
	"show", 0, 0, 0, 0, ch_show, ACT_NONE,
	"Show running system information"
};
static __code const struct cli_node n_write = {
	"write", 0, CLI_F_PRIV, 0, 0, ch_write, ACT_WRITE,
	"Write the running configuration to flash"
};
static __code const struct cli_node n_copy = {
	"copy", 0, CLI_F_PRIV, 0, 0, ch_copy, ACT_NONE,
	"Copy configuration or image data"
};
static __code const struct cli_node n_reload = {
	"reload", 0, CLI_F_PRIV, 0, 0, NO_CHILDREN, ACT_RELOAD,
	"Halt and perform a cold restart"
};
static __code const struct cli_node n_exit_exec = {
	"exit", 0, 0, 0, 0, NO_CHILDREN, ACT_EXIT,
	"Exit the current mode"
};

static __code const struct cli_node * __code const cli_root_exec[] = {
	&n_configure, &n_copy, &n_disable, &n_enable, &n_exit_exec,
	&n_reload, &n_show, &n_write, 0
};

/* ---- global configuration mode ---- */

/* interface ethernet S/N | interface vlan N | interface S/N */
static __code const struct cli_node n_arg_ifnum = {
	0, CLI_A_IFACE, 0, 0, 0, NO_CHILDREN, ACT_IF,
	"Interface number (e.g. 1/5)"
};
static __code const struct cli_node * __code const ch_iface[] = {
	&n_arg_ifnum, 0
};
static __code const struct cli_node n_if_ethernet = {
	"ethernet", 0, 0, 0, 0, ch_iface, ACT_NONE,
	"Ethernet interface"
};
static __code const struct cli_node n_arg_svi = {
	0, CLI_A_NUM, 0, 1, 4094, NO_CHILDREN, ACT_SVI,
	"VLAN interface number"
};
static __code const struct cli_node * __code const ch_ifvlan[] = {
	&n_arg_svi, 0
};
static __code const struct cli_node n_if_vlan = {
	"vlan", 0, 0, 0, 0, ch_ifvlan, ACT_NONE,
	"VLAN (management) interface"
};
static __code const struct cli_node * __code const ch_interface[] = {
	&n_if_ethernet, &n_if_vlan, &n_arg_ifnum, 0
};
static __code const struct cli_node n_interface = {
	"interface", 0, 0, 0, 0, ch_interface, ACT_NONE,
	"Select an interface to configure"
};

/* vlan N */
static __code const struct cli_node n_arg_vlanid = {
	0, CLI_A_NUM, CLI_F_NO_OK, 1, 4094, NO_CHILDREN, ACT_VLAN,
	"VLAN id"
};
static __code const struct cli_node * __code const ch_vlan[] = {
	&n_arg_vlanid, 0
};
static __code const struct cli_node n_vlan = {
	"vlan", 0, 0, 0, 0, ch_vlan, ACT_NONE,
	"Add, delete or modify a VLAN"
};

/* hostname WORD */
static __code const struct cli_node n_arg_hostname = {
	0, CLI_A_WORD, 0, 0, 0, NO_CHILDREN, ACT_HOSTNAME,
	"Up to 23 characters"
};
static __code const struct cli_node * __code const ch_hostname[] = {
	&n_arg_hostname, 0
};
static __code const struct cli_node n_hostname = {
	"hostname", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, ch_hostname, ACT_HOSTNAME,
	"Set the system name"
};

/* ip default-gateway A.B.C.D | ip igmp snooping */
static __code const struct cli_node n_arg_gw = {
	0, CLI_A_IP, CLI_F_NO_OK, 0, 0, NO_CHILDREN, ACT_DEFGW,
	"Gateway address"
};
static __code const struct cli_node * __code const ch_gw[] = {
	&n_arg_gw, 0
};
static __code const struct cli_node n_ip_defgw = {
	"default-gateway", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, ch_gw, ACT_DEFGW,
	"Default gateway"
};
static __code const struct cli_node n_igmp_snooping = {
	"snooping", 0, CLI_F_NO_OK, 0, 0, NO_CHILDREN, ACT_IGMP,
	"IGMP snooping"
};
static __code const struct cli_node * __code const ch_igmp[] = {
	&n_igmp_snooping, 0
};
static __code const struct cli_node n_ip_igmp = {
	"igmp", 0, 0, 0, 0, ch_igmp, ACT_NONE,
	"IGMP configuration"
};
static __code const struct cli_node * __code const ch_ip_cfg[] = {
	&n_ip_defgw, &n_ip_igmp, 0
};
static __code const struct cli_node n_ip_cfg = {
	"ip", 0, 0, 0, 0, ch_ip_cfg, ACT_NONE,
	"Global IP configuration"
};

/* logging host A.B.C.D [port N] */
static __code const struct cli_node n_arg_logport = {
	0, CLI_A_NUM, 0, 1, 65535, NO_CHILDREN, ACT_LOG_HOST,
	"UDP port"
};
static __code const struct cli_node * __code const ch_logport[] = {
	&n_arg_logport, 0
};
static __code const struct cli_node n_log_port = {
	"port", 0, 0, 0, 0, ch_logport, ACT_NONE,
	"Syslog server port (default 514)"
};
static __code const struct cli_node * __code const ch_loghost_ip[] = {
	&n_log_port, 0
};
static __code const struct cli_node n_arg_loghost = {
	0, CLI_A_IP, CLI_F_NO_OK, 0, 0, ch_loghost_ip, ACT_LOG_HOST,
	"Syslog server address"
};
static __code const struct cli_node * __code const ch_loghost[] = {
	&n_arg_loghost, 0
};
static __code const struct cli_node n_log_host = {
	"host", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, ch_loghost, ACT_LOG_HOST,
	"Remote syslog server"
};
static __code const struct cli_node * __code const ch_logging[] = {
	&n_log_host, 0
};
static __code const struct cli_node n_logging = {
	"logging", 0, 0, 0, 0, ch_logging, ACT_NONE,
	"Message logging"
};

static __code const struct cli_node n_exit_cfg = {
	"exit", 0, 0, 0, 0, NO_CHILDREN, ACT_EXIT,
	"Exit the current mode"
};
static __code const struct cli_node n_end = {
	"end", 0, 0, 0, 0, NO_CHILDREN, ACT_END,
	"Exit to privileged EXEC mode"
};

static __code const struct cli_node * __code const cli_root_config[] = {
	&n_end, &n_exit_cfg, &n_hostname, &n_interface, &n_ip_cfg,
	&n_logging, &n_vlan, 0
};

/* ---- interface configuration mode ---- */
static __code const struct cli_node n_if_shutdown = {
	"shutdown", 0, CLI_F_NO_OK, 0, 0, NO_CHILDREN, ACT_SHUT,
	"Disable the interface"
};
/* speed <value>: each literal carries its PHY_SPEED_* code in ->lo */
static __code const struct cli_node n_sp_auto = {
	"auto", 0, 0, PHY_SPEED_AUTO, 0, NO_CHILDREN, ACT_SPEED, "Autonegotiate"
};
static __code const struct cli_node n_sp_10 = {
	"10", 0, 0, PHY_SPEED_10M, 0, NO_CHILDREN, ACT_SPEED, "10 Mb/s"
};
static __code const struct cli_node n_sp_100 = {
	"100", 0, 0, PHY_SPEED_100M, 0, NO_CHILDREN, ACT_SPEED, "100 Mb/s"
};
static __code const struct cli_node n_sp_1000 = {
	"1000", 0, 0, PHY_SPEED_1G, 0, NO_CHILDREN, ACT_SPEED, "1 Gb/s"
};
static __code const struct cli_node n_sp_2500 = {
	"2500", 0, 0, PHY_SPEED_2G5, 0, NO_CHILDREN, ACT_SPEED, "2.5 Gb/s"
};
static __code const struct cli_node n_sp_5000 = {
	"5000", 0, 0, PHY_SPEED_5G, 0, NO_CHILDREN, ACT_SPEED, "5 Gb/s"
};
static __code const struct cli_node n_sp_10000 = {
	"10000", 0, 0, PHY_SPEED_10G, 0, NO_CHILDREN, ACT_SPEED, "10 Gb/s"
};
static __code const struct cli_node * __code const ch_speed[] = {
	&n_sp_auto, &n_sp_10, &n_sp_100, &n_sp_1000, &n_sp_2500,
	&n_sp_5000, &n_sp_10000, 0
};
static __code const struct cli_node n_if_speed = {
	"speed", 0, 0, 0, 0, ch_speed, ACT_NONE,
	"Set the interface speed"
};
static __code const struct cli_node n_arg_desc = {
	0, CLI_A_LINE, 0, 0, 0, NO_CHILDREN, ACT_DESC, "Up to 31 characters"
};
static __code const struct cli_node * __code const ch_desc[] = {
	&n_arg_desc, 0
};
static __code const struct cli_node n_if_description = {
	"description", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, ch_desc, ACT_DESC,
	"Interface description / name"
};

/* mtu N */
static __code const struct cli_node n_arg_mtu = {
	0, CLI_A_NUM, 0, 64, 16383, NO_CHILDREN, ACT_MTU,
	"Maximum frame length in bytes"
};
static __code const struct cli_node * __code const ch_mtu[] = {
	&n_arg_mtu, 0
};
static __code const struct cli_node n_if_mtu = {
	"mtu", 0, 0, 0, 0, ch_mtu, ACT_NONE,
	"Set the maximum frame length"
};

/* switchport mode access|trunk */
static __code const struct cli_node n_swm_access = {
	"access", 0, 0, SW_MODE_ACCESS, 0, NO_CHILDREN, ACT_SW_MODE,
	"Untagged member of one VLAN"
};
static __code const struct cli_node n_swm_trunk = {
	"trunk", 0, 0, SW_MODE_TRUNK, 0, NO_CHILDREN, ACT_SW_MODE,
	"Tagged member of several VLANs"
};
static __code const struct cli_node * __code const ch_swmode[] = {
	&n_swm_access, &n_swm_trunk, 0
};
static __code const struct cli_node n_sw_mode = {
	"mode", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, SW_MODE_ACCESS, 0, ch_swmode, ACT_SW_MODE,
	"Set the switchport mode"
};

/* switchport access vlan N */
static __code const struct cli_node n_swa_vid = {
	0, CLI_A_NUM, CLI_F_NO_OK, 1, 4094, NO_CHILDREN, ACT_SW_ACCESS,
	"VLAN id"
};
static __code const struct cli_node * __code const ch_swa_vlan[] = {
	&n_swa_vid, 0
};
static __code const struct cli_node n_swa_vlan = {
	"vlan", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, ch_swa_vlan, ACT_SW_ACCESS,
	"Access VLAN"
};
static __code const struct cli_node * __code const ch_swaccess[] = {
	&n_swa_vlan, 0
};
static __code const struct cli_node n_sw_access = {
	"access", 0, 0, 0, 0, ch_swaccess, ACT_NONE,
	"Access mode characteristics"
};

/* switchport trunk native vlan N */
static __code const struct cli_node n_swn_vid = {
	0, CLI_A_NUM, CLI_F_NO_OK, 1, 4094, NO_CHILDREN, ACT_SW_NATIVE,
	"VLAN id"
};
static __code const struct cli_node * __code const ch_swn_vlan[] = {
	&n_swn_vid, 0
};
static __code const struct cli_node n_swn_vlan = {
	"vlan", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, ch_swn_vlan, ACT_SW_NATIVE,
	"Native (untagged) VLAN"
};
static __code const struct cli_node * __code const ch_swt_native[] = {
	&n_swn_vlan, 0
};
static __code const struct cli_node n_swt_native = {
	"native", 0, 0, 0, 0, ch_swt_native, ACT_NONE,
	"Native VLAN of the trunk"
};

/* switchport trunk allowed vlan all|none|LIST|add LIST|remove LIST
 * (the operation is carried in ->lo) */
static __code const struct cli_node n_al_all = {
	"all", 0, 0, SW_AL_ALL, 0, NO_CHILDREN, ACT_SW_ALLOWED, "All VLANs"
};
static __code const struct cli_node n_al_none = {
	"none", 0, 0, SW_AL_NONE, 0, NO_CHILDREN, ACT_SW_ALLOWED, "No VLANs"
};
static __code const struct cli_node n_arg_al_add = {
	0, CLI_A_WORD, 0, SW_AL_ADD, 0, NO_CHILDREN, ACT_SW_ALLOWED,
	"VLAN list, e.g. 10,20-30"
};
static __code const struct cli_node * __code const ch_al_add[] = {
	&n_arg_al_add, 0
};
static __code const struct cli_node n_al_add = {
	"add", 0, 0, 0, 0, ch_al_add, ACT_NONE, "Add VLANs to the list"
};
static __code const struct cli_node n_arg_al_rem = {
	0, CLI_A_WORD, 0, SW_AL_REMOVE, 0, NO_CHILDREN, ACT_SW_ALLOWED,
	"VLAN list, e.g. 10,20-30"
};
static __code const struct cli_node * __code const ch_al_rem[] = {
	&n_arg_al_rem, 0
};
static __code const struct cli_node n_al_remove = {
	"remove", 0, 0, 0, 0, ch_al_rem, ACT_NONE, "Remove VLANs from the list"
};
static __code const struct cli_node n_arg_al_set = {
	0, CLI_A_WORD, 0, SW_AL_SET, 0, NO_CHILDREN, ACT_SW_ALLOWED,
	"VLAN list, e.g. 10,20-30"
};
static __code const struct cli_node * __code const ch_al_vlan[] = {
	&n_al_add, &n_al_all, &n_al_none, &n_al_remove, &n_arg_al_set, 0
};
static __code const struct cli_node n_swal_vlan = {
	"vlan", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, SW_AL_ALL, 0, ch_al_vlan, ACT_SW_ALLOWED,
	"VLANs carried by the trunk"
};
static __code const struct cli_node * __code const ch_swt_allowed[] = {
	&n_swal_vlan, 0
};
static __code const struct cli_node n_swt_allowed = {
	"allowed", 0, 0, 0, 0, ch_swt_allowed, ACT_NONE,
	"Allowed VLAN list"
};
static __code const struct cli_node * __code const ch_swtrunk[] = {
	&n_swt_allowed, &n_swt_native, 0
};
static __code const struct cli_node n_sw_trunk = {
	"trunk", 0, 0, 0, 0, ch_swtrunk, ACT_NONE,
	"Trunk mode characteristics"
};
static __code const struct cli_node * __code const ch_switchport[] = {
	&n_sw_access, &n_sw_mode, &n_sw_trunk, 0
};
static __code const struct cli_node n_if_switchport = {
	"switchport", 0, 0, 0, 0, ch_switchport, ACT_NONE,
	"Set switching mode characteristics"
};

static __code const struct cli_node * __code const cli_root_if[] = {
	&n_end, &n_exit_cfg, &n_if_description, &n_if_mtu, &n_if_shutdown,
	&n_if_speed, &n_if_switchport, 0
};

/* ---- VLAN configuration mode ---- */
static __code const struct cli_node n_arg_vname = {
	0, CLI_A_WORD, 0, 0, 0, NO_CHILDREN, ACT_VLAN_NAME,
	"Up to 32 characters"
};
static __code const struct cli_node * __code const ch_vname[] = {
	&n_arg_vname, 0
};
static __code const struct cli_node n_vl_name = {
	"name", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, ch_vname, ACT_VLAN_NAME,
	"Name of the VLAN"
};
static __code const struct cli_node * __code const cli_root_vlan[] = {
	&n_end, &n_exit_cfg, &n_vl_name, 0
};

/* ---- VLAN (management) interface mode ---- */
static __code const struct cli_node n_arg_ip_mask = {
	0, CLI_A_IP, 0, 0, 0, NO_CHILDREN, ACT_IP_ADDR,
	"Subnet mask"
};
static __code const struct cli_node * __code const ch_ip_mask[] = {
	&n_arg_ip_mask, 0
};
static __code const struct cli_node n_arg_ip_addr = {
	0, CLI_A_IP, 0, 0, 0, ch_ip_mask, ACT_NONE,
	"IP address"
};
static __code const struct cli_node n_ip_dhcp = {
	"dhcp", 0, 0, 0, 0, NO_CHILDREN, ACT_IP_DHCP,
	"Obtain the address with DHCP"
};
static __code const struct cli_node * __code const ch_ipaddr[] = {
	&n_ip_dhcp, &n_arg_ip_addr, 0
};
static __code const struct cli_node n_svi_address = {
	"address", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, ch_ipaddr, ACT_IP_ADDR,
	"Set the interface IP address"
};
static __code const struct cli_node * __code const ch_svi_ip[] = {
	&n_svi_address, 0
};
static __code const struct cli_node n_svi_ip = {
	"ip", 0, 0, 0, 0, ch_svi_ip, ACT_NONE,
	"Interface IP configuration"
};
static __code const struct cli_node * __code const cli_root_svi[] = {
	&n_end, &n_exit_cfg, &n_svi_ip, 0
};

static __code const struct cli_node * __code const cli_root_sub[] = {
	&n_end, &n_exit_cfg, 0
};


static __code const struct cli_node * __code const *root_for_mode(uint8_t mode)
{
	switch (mode) {
	case CLI_MODE_CONFIG:
		return cli_root_config;
	case CLI_MODE_IF:
		return cli_root_if;
	case CLI_MODE_VLAN:
		return cli_root_vlan;
	case CLI_MODE_SVI:
		return cli_root_svi;
	case CLI_MODE_LINE:
		return cli_root_sub;
	default:
		return cli_root_exec;
	}
}


/* ---------------- helpers ---------------- */

static char lc(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c | 0x20;
	return c;
}


static void cli_tokenize(__xdata char *line)
{
	__xdata uint8_t i = 0;

	cli_line = line;
	ntok = 0;
	trailing_space = 1;
	while (line[i]) {
		if (line[i] == ' ') {
			i++;
			continue;
		}
		if (ntok >= CLI_MAX_TOKS)
			break;
		tok_off[ntok] = i;
		while (line[i] && line[i] != ' ')
			i++;
		tok_len[ntok] = i - tok_off[ntok];
		ntok++;
		trailing_space = (line[i] == ' ') ? 1 : 0;
	}
	if (!ntok)
		trailing_space = 1;
}


/* Is token t a prefix of the code-space word? 2 = exact match. */
static __xdata char *tm_p;
static __xdata uint8_t tm_n;

static uint8_t tok_matches(uint8_t t, __code const char *word)
{
	tm_p = cli_line + tok_off[t];
	tm_n = tok_len[t];

	while (tm_n) {
		if (!*word || lc(*tm_p) != *word)
			return 0;
		tm_p++;
		word++;
		tm_n--;
	}
	return *word ? 1 : 2;
}


static uint8_t node_visible(__code const struct cli_node *n)
{
	if ((n->flags & CLI_F_PRIV) && cli.mode == CLI_MODE_EXEC)
		return 0;
	if ((n->flags & CLI_F_NO_ONLY) && !cli.no)
		return 0;
	return 1;
}


/* Validate token t against an argument node; store the value. Returns 0
 * on mismatch. */
static uint8_t arg_accept(uint8_t t, __code const struct cli_node *n)
{
	__xdata char *p = cli_line + tok_off[t];
	__xdata uint8_t len = tok_len[t];
	__xdata uint32_t v = 0;
	__xdata uint8_t i = 0;

	if (cli.nargs >= CLI_MAX_ARGS)
		return 0;

	switch (n->arg) {
	case CLI_A_NUM:
		if (!len || len > 5)
			return 0;
		for (i = 0; i < len; i++) {
			if (p[i] < '0' || p[i] > '9')
				return 0;
			v = v * 10 + (p[i] - '0');
		}
		if (v < n->lo || v > n->hi)
			return 0;
		break;
	case CLI_A_IP:
	{
		static __xdata uint8_t dots, digits;
		static __xdata uint16_t oct;
		dots = 0; digits = 0; oct = 0;
		v = 0;
		for (i = 0; i < len; i++) {
			if (p[i] == '.') {
				if (!digits || dots == 3)
					return 0;
				v = (v << 8) | oct;
				oct = 0;
				digits = 0;
				dots++;
			} else if (p[i] >= '0' && p[i] <= '9') {
				oct = oct * 10 + (p[i] - '0');
				if (oct > 255 || ++digits > 3)
					return 0;
			} else {
				return 0;
			}
		}
		if (dots != 3 || !digits)
			return 0;
		v = (v << 8) | oct;
		break;
	}
	case CLI_A_IFACE:
	{
		/* [ethernet-prefix]N or [ethernet-prefix]S/N; the port is N */
		static __code const char * __xdata w;
		static __xdata uint16_t num;
		static __xdata uint8_t have;
		w = "ethernet"; num = 0; have = 0;
		i = 0;
		while (i < len && ((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z'))) {
			if (!*w || lc(p[i]) != *w)
				return 0;
			w++;
			i++;
		}
		for (; i < len; i++) {
			if (p[i] == '/') {
				if (!have)
					return 0;
				num = 0;
				have = 0;
			} else if (p[i] >= '0' && p[i] <= '9') {
				num = num * 10 + (p[i] - '0');
				have = 1;
				if (num > 99)
					return 0;
			} else {
				return 0;
			}
		}
		if (!have || num < 1 || num > 9)
			return 0;
		v = num;
		break;
	}
	case CLI_A_WORD:
		if (!len)
			return 0;
		break;
	case CLI_A_LINE:
		break;
	default:
		return 0;
	}

	cli.args[cli.nargs] = v;
	cli.argoff[cli.nargs] = tok_off[t];
	cli.nargs++;
	return 1;
}


/* Walk tokens [0, upto) through the given root. */
static void cli_walk(__code const struct cli_node * __code const *root, uint8_t upto)
{
	__xdata uint8_t t;

	w_children = root;
	w_node = 0;
	w_status = W_OK;
	w_badtok = 0;
	cli.nargs = 0;

	for (t = 0; t < upto; t++) {
		__code const struct cli_node * __xdata exact = 0;
		__code const struct cli_node * __xdata pref = 0;
		__xdata uint8_t npref = 0;
		__code const struct cli_node * __code const * __xdata c;
		__code const struct cli_node * __xdata sel = 0;

		if (!w_children) {
			w_status = W_INVALID;
			w_badtok = t;
			return;
		}
		for (c = w_children; *c; c++) {
			if (!node_visible(*c))
				continue;
			if ((*c)->word) {
				static __xdata uint8_t m;
				m = tok_matches(t, (*c)->word);
				if (m == 2)
					exact = *c;
				else if (m == 1) {
					pref = *c;
					npref++;
				}
			}
		}
		if (exact)
			sel = exact;
		else if (npref == 1)
			sel = pref;
		else if (npref > 1) {
			w_status = W_AMBIG;
			w_badtok = t;
			return;
		}
		if (!sel) {
			/* argument placeholders, in table order */
			for (c = w_children; *c; c++) {
				if (!node_visible(*c) || (*c)->word)
					continue;
				if (arg_accept(t, *c)) {
					sel = *c;
					break;
				}
			}
		}
		if (!sel) {
			w_status = (t == 0) ? W_NOMATCH0 : W_INVALID;
			w_badtok = t;
			return;
		}
		w_node = sel;
		if (!sel->word && sel->arg == CLI_A_LINE)
			return;	/* swallows the rest of the line */
		w_children = sel->children;
	}
}


/* ---------------- output helpers ---------------- */

static void cli_spaces(uint8_t n)
{
	while (n--)
		write_char(' ');
}


static void cli_marker_error(void)
{
	cli_spaces(cli_plen + tok_off[w_badtok]);
	print_string("^\n% Invalid input detected at '^' marker.\n\n");
}


static void print_placeholder(__code const struct cli_node *n)
{
	switch (n->arg) {
	case CLI_A_NUM:
		write_char('<');
		itoa_short(n->lo);
		write_char('-');
		itoa_short(n->hi);
		write_char('>');
		break;
	case CLI_A_IP:
		print_string("A.B.C.D");
		break;
	case CLI_A_IFACE:
		print_string("<1-9> or S/N");
		break;
	case CLI_A_WORD:
		print_string("WORD");
		break;
	case CLI_A_LINE:
		print_string("LINE");
		break;
	}
}


static __xdata uint8_t csl_n;

static uint8_t code_strlen(__code const char *s)
{
	csl_n = 0;
	while (*s++)
		csl_n++;
	return csl_n;
}


/* When help lists the EXEC root behind a config-mode root, skip words
 * the mode root already listed (exit/end live in both). */
static __code const struct cli_node * __code const * __xdata dedupe_root;

static uint8_t code_streq(__code const char *a, __code const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static uint8_t word_in_dedupe_root(__code const char *w)
{
	__code const struct cli_node * __code const * __xdata c;

	if (!dedupe_root)
		return 0;
	for (c = dedupe_root; *c; c++) {
		if ((*c)->word && code_streq((*c)->word, w))
			return 1;
	}
	return 0;
}


/* Would <cr> execute the node reached so far? */
static uint8_t node_runs_here(__code const struct cli_node * __xdata n)
{
	if (!n || !n->action)
		return 0;
	if ((n->flags & CLI_F_NO_EXEC) && !cli.no)
		return 0;
	return 1;
}


static void cli_list_candidates(uint8_t partial_tok)
{
	__code const struct cli_node * __code const * __xdata c;
	__xdata uint8_t width = 0;
	__xdata uint8_t any = 0;

	if (!w_children) {
		if (node_runs_here(w_node))
			print_string("  <cr>\n");
		return;
	}
	/* column width from the longest literal */
	for (c = w_children; *c; c++) {
		if (!node_visible(*c))
			continue;
		if ((*c)->word) {
			static __xdata uint8_t n;
			n = code_strlen((*c)->word);
			if (n > width)
				width = n;
		}
	}
	if (width < 14)
		width = 14;
	for (c = w_children; *c; c++) {
		if (!node_visible(*c))
			continue;
		if (partial_tok != 0xff && (*c)->word
		    && !tok_matches(partial_tok, (*c)->word))
			continue;
		if ((*c)->word && word_in_dedupe_root((*c)->word))
			continue;
		write_char(' ');
		write_char(' ');
		if ((*c)->word) {
			static __xdata uint8_t n;
			n = code_strlen((*c)->word);
			print_string((*c)->word);
			cli_spaces(width - n + 2);
		} else {
			print_placeholder(*c);
			print_string("  ");
		}
		if ((*c)->help)
			print_string((*c)->help);
		write_char('\n');
		any = 1;
	}
	if (node_runs_here(w_node) && partial_tok == 0xff)
		print_string("  <cr>\n");
	if (!any && partial_tok != 0xff)
		print_string("% Unrecognized command\n");
}


/* ---------------- actions ---------------- */

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


static void cli_dispatch(uint8_t action)
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
	case ACT_SHOW_VER:
		print_sw_version();
		break;
	case ACT_WRITE:
		print_string("Building configuration...\n");
		cmd_save_config();
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
		if (d_v < 1 || d_v > 9) {
			print_string("% Invalid interface\n");
			break;
		}
		d_lp = machine.phys_to_log_port[d_v - 1];
		if (d_lp < machine.min_port || d_lp > machine.max_port) {
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
		if (!name_ok(cli_line + cli.argoff[0])) {
			print_string("% Invalid name\n");
			break;
		}
		d_rc = sw_vlan_name_set(cli.ctx_vlan, cli_line + cli.argoff[0]);
		if (d_rc)
			print_string("% VLAN name table full\n");
		break;
	case ACT_LEGACY:
		execute_commands((__xdata uint8_t *)cli_line);
		break;
	case ACT_SHUT:
		phy_settings.port = cli.ctx_lport;
		phy_settings.duplex = PHY_DUPLEX_BOTH;
		phy_settings.speed = cli.no ? PHY_SPEED_AUTO : PHY_OFF;
		phy_set_speed();
		break;
	case ACT_SPEED:
		phy_settings.port = cli.ctx_lport;
		phy_settings.duplex = PHY_DUPLEX_BOTH;
		phy_settings.speed = w_node->lo;
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
		ds = cli_line + cli.argoff[0];
		for (dn = 0; *ds && dn < PORT_NAME_SIZE - 1; dn++)
			*dd++ = *ds++;
		*dd = 0;
		break;
	}
	case ACT_MTU:
		sw_mtu_set(cli.ctx_lport, cli.args[0]);
		break;
	case ACT_SW_MODE:
		sw_ports[cli.ctx_lport].mode = cli.no ? SW_MODE_ACCESS : w_node->lo;
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
		d_rc = cli.no ? SW_AL_ALL : w_node->lo;
		d_rc = sw_allowed_edit(cli.ctx_lport, d_rc,
				       cli.nargs ? cli_line + cli.argoff[0] : 0);
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
		hs = cli_line + cli.argoff[0];
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
	}
}


/* ---------------- public entry points ---------------- */

void cli_init(void) __banked
{
	cli.mode = CLI_MODE_EXEC;
	cli.await = CLI_AWAIT_NONE;
	cli.no = 0;
}


uint8_t cli_hidden_input(void) __banked
{
	return cli.await != CLI_AWAIT_NONE;
}


/* A leading `no` in a configuration mode: set cli.no and drop the
 * token. Exact match only, so a lone "n" is not taken for "no". */
static void cli_strip_no(void)
{
	static __xdata uint8_t i;

	cli.no = 0;
	if (cli.mode < CLI_MODE_CONFIG || !ntok || tok_matches(0, "no") != 2)
		return;
	cli.no = 1;
	for (i = 1; i < ntok; i++) {
		tok_off[i - 1] = tok_off[i];
		tok_len[i - 1] = tok_len[i];
	}
	ntok--;
}


/* Match the tokenized line against the mode root, then the EXEC root
 * (commands-anywhere), for tokens [start, upto). Returns 1 when some
 * root accepted the first token. */
static __xdata uint8_t w_level;	/* root that matched: WL_* */
#define WL_MODE		0	/* the current mode's own root */
#define WL_PARENT	1	/* global config, reached from a submode */
#define WL_EXEC		2	/* EXEC, reached from a config mode */

static uint8_t walk_ok(void)
{
	return w_status == W_OK && node_runs_here(w_node);
}

/* Resolution order, as in the industry-standard CLIs:
 *  1. the current mode's root;
 *  2. from a submode, global config: a line that does not parse in
 *     config-if/-vlan runs as a global command and leaves the submode.
 *     This is what lets a block-structured config replay without `exit`
 *     lines. Tried whenever step 1 did not yield something runnable;
 *     its error wins only when step 1 did not even know the first word;
 *  3. EXEC commands from any config mode, without `do` (never under
 *     `no`), when nothing claimed the first word.
 * Returns 0 when no root claims the first word (-> legacy parser). */
static uint8_t cli_walk_roots(uint8_t upto)
{
	static __xdata uint8_t r0;

	w_level = WL_MODE;
	cli_walk(root_for_mode(cli.mode), upto);
	if (walk_ok())
		return 1;
	r0 = w_status;
	if (cli.mode > CLI_MODE_CONFIG) {
		cli_walk(cli_root_config, upto);
		if (walk_ok() || (r0 == W_NOMATCH0 && w_status != W_NOMATCH0)) {
			w_level = WL_PARENT;
			return 1;
		}
		/* report relative to the submode */
		cli_walk(root_for_mode(cli.mode), upto);
	}
	if (w_status == W_NOMATCH0 && cli.mode >= CLI_MODE_CONFIG && !cli.no) {
		cli_walk(cli_root_exec, upto);
		w_level = WL_EXEC;
	}
	return w_status != W_NOMATCH0;
}


void cli_exec_line(__xdata char *line) __banked
{
	cli_tokenize(line);
	cli.no = 0;

	if (!ntok)
		return;

	cli_strip_no();
	if (cli.no && !ntok) {
		print_string("% Incomplete command.\n\n");
		return;
	}

	if (!cli_walk_roots(ntok)) {
		/* Nothing in the tree claims this line: legacy parser */
		execute_commands((__xdata uint8_t *)line);
		return;
	}

	switch (w_status) {
	case W_AMBIG:
		print_string("% Ambiguous command:  \"");
		print_string_x((__xdata char *)line);
		print_string("\"\n\n");
		return;
	case W_INVALID:
		cli_marker_error();
		return;
	}

	if (!w_node || !w_node->action) {
		print_string("% Incomplete command.\n\n");
		return;
	}
	if (cli.no && !(w_node->flags & (CLI_F_NO_OK | CLI_F_NO_ONLY))) {
		cli_marker_error();
		return;
	}
	if (!cli.no && (w_node->flags & CLI_F_NO_EXEC)) {
		print_string("% Incomplete command.\n\n");
		return;
	}
	if (w_level == WL_PARENT)
		cli.mode = CLI_MODE_CONFIG;
	cli_dispatch(w_node->action);
}


/* No leading newline: callers position the cursor (fresh line after a
 * command, or an in-place redraw from the line editor). */
void cli_prompt(void) __banked
{
	__xdata char *h = hostname;

	cli_plen = 0;
	while (*h) {
		write_char(*h++);
		cli_plen++;
	}
	switch (cli.mode) {
	case CLI_MODE_EXEC:
		print_string("> ");
		cli_plen += 2;
		return;
	case CLI_MODE_CONFIG:
		print_string("(config)");
		cli_plen += 8;
		break;
	case CLI_MODE_IF:
	case CLI_MODE_SVI:
		print_string("(config-if)");
		cli_plen += 11;
		break;
	case CLI_MODE_VLAN:
		print_string("(config-vlan)");
		cli_plen += 13;
		break;
	case CLI_MODE_LINE:
		print_string("(config-line)");
		cli_plen += 13;
		break;
	}
	print_string("# ");
	cli_plen += 2;
}


void cli_help(__xdata char *line) __banked
{
	cli_tokenize(line);
	cli_strip_no();

	if (!ntok || (ntok == 1 && !trailing_space)) {
		/* listing the root: match nothing, list all (filtered by a
		 * partial first token if present) */
		w_children = root_for_mode(cli.mode);
		w_node = 0;
		cli.nargs = 0;
		dedupe_root = 0;
		cli_list_candidates(ntok ? 0 : 0xff);
		if (cli.mode >= CLI_MODE_CONFIG) {
			dedupe_root = root_for_mode(cli.mode);
			w_children = cli_root_exec;
			cli_list_candidates(ntok ? 0 : 0xff);
			dedupe_root = 0;
		}
		return;
	}

	if (trailing_space) {
		/* all tokens complete: list children of the match */
		if (!cli_walk_roots(ntok) || w_status != W_OK) {
			print_string("% Unrecognized command\n");
			return;
		}
		cli_list_candidates(0xff);
	} else {
		/* last token partial: filter its candidates */
		if (!cli_walk_roots(ntok - 1) || w_status != W_OK) {
			print_string("% Unrecognized command\n");
			return;
		}
		cli_list_candidates(ntok - 1);
	}
}


static void cplt_scan(__code const struct cli_node * __code const * __xdata root,
		      __code const struct cli_node * __xdata * __xdata cand,
		      __xdata uint8_t * __xdata ncand)
{
	static __code const struct cli_node * __code const * __xdata c;

	for (c = root; *c; c++) {
		if (!node_visible(*c) || !(*c)->word)
			continue;
		if (!tok_matches(ntok - 1, (*c)->word))
			continue;
		if (*cand && code_streq((*cand)->word, (*c)->word))
			continue;	/* same word from another root */
		*cand = *c;
		(*ncand)++;
	}
}


uint8_t cli_complete(__xdata char *line, uint8_t maxlen) __banked
{
	__code const struct cli_node * __xdata cand = 0;
	__xdata uint8_t ncand = 0;
	__xdata uint8_t len, added = 0;
	static __code const char * __xdata w;

	cli_tokenize(line);
	cli_strip_no();
	if (!ntok || trailing_space)
		return 0;

	if (ntok == 1) {
		w_children = root_for_mode(cli.mode);
	} else {
		if (!cli_walk_roots(ntok - 1) || w_status != W_OK || !w_children)
			return 0;
	}
	cplt_scan(w_children, &cand, &ncand);
	/* the first token may also complete from global config (in a
	 * submode) and from EXEC (in any config mode) */
	if (ntok == 1 && cli.mode > CLI_MODE_CONFIG)
		cplt_scan(cli_root_config, &cand, &ncand);
	if (ntok == 1 && cli.mode >= CLI_MODE_CONFIG && !cli.no)
		cplt_scan(cli_root_exec, &cand, &ncand);
	if (ncand != 1)
		return 0;

	len = tok_len[ntok - 1];
	w = cand->word + len;
	while (*w && (uint8_t)(tok_off[ntok - 1] + len + added) < maxlen - 2) {
		line[tok_off[ntok - 1] + len + added] = *w++;
		added++;
	}
	line[tok_off[ntok - 1] + len + added] = ' ';
	added++;
	line[tok_off[ntok - 1] + len + added] = 0;
	return added;
}
