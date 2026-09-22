/*
 * Modal CLI engine. See cli.h for the model.
 *
 * The command tree lives in code space; matching walks one token at a
 * time with unique-prefix abbreviation. Literal tokens win over
 * argument placeholders. EXEC commands are reachable from config modes
 * without `do` (mode root first, EXEC root second).
 *
 * The serial console and the telnet vty are separate sessions: each
 * keeps its own mode and submode context, swapped in by cli_use().
 *
 * Handlers are action ids dispatched from a switch, not function
 * pointers: banked function pointers are fragile with SDCC, ids are
 * free.
 */
#include <stddef.h>
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

#pragma codeseg BANK1
#pragma constseg BANK1

extern __xdata char hostname[24];
extern __xdata struct phy_settings phy_settings;
extern __code const struct machine machine;
extern __xdata uint16_t management_vlan;
extern __xdata char passwd[21];
void reset_chip(void);

__xdata struct cli_state_t cli;

static __xdata uint8_t cli_replaying;
/* When help lists the EXEC root behind a config-mode root, skip words
 * the mode root already listed (exit/end live in both). */
static __code const struct cli_node * __code const * __xdata dedupe_root;

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

#include "cli_act.h"
#include "dbgcmd.h"
#include "tftp.h"

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
	"version", 0, 0, SHOW_VER, 0, NO_CHILDREN, ACT_SHOW,
	"Software, hardware and uptime"
};
static __code const struct cli_node n_show_run = {
	"running-config", 0, 0, 0, 0, NO_CHILDREN, ACT_SHOW_RUN,
	"Current operating configuration"
};
static __code const struct cli_node n_show_start = {
	"startup-config", 0, 0, 0, 0, NO_CHILDREN, ACT_SHOW_START,
	"Configuration used at boot"
};
#define SHOW_LEAF(nm, word, code, help) \
static __code const struct cli_node nm = { \
	word, 0, 0, code, 0, NO_CHILDREN, ACT_SHOW, help \
};
SHOW_LEAF(n_sh_if_status, "status", SHOW_IF_STATUS, "Link, VLAN and speed per port")
SHOW_LEAF(n_sh_if_count, "counters", SHOW_IF_COUNT, "Packet and error counters")
SHOW_LEAF(n_sh_if_trunk, "trunk", SHOW_IF_TRUNK, "Trunk ports")
SHOW_LEAF(n_sh_if_xcvr, "transceiver", SHOW_IF_XCVR, "SFP modules and diagnostics")
static __code const struct cli_node * __code const ch_sh_if[] = {
	&n_sh_if_count, &n_sh_if_status, &n_sh_if_xcvr, &n_sh_if_trunk, 0
};
static __code const struct cli_node n_sh_if = {
	"interfaces", 0, 0, SHOW_IF_STATUS, 0, ch_sh_if, ACT_SHOW, "Interface status"
};
SHOW_LEAF(n_sh_vlan_brief, "brief", SHOW_VLAN, "One line per VLAN")
static __code const struct cli_node * __code const ch_sh_vlan[] = {
	&n_sh_vlan_brief, 0
};
static __code const struct cli_node n_sh_vlan = {
	"vlan", 0, 0, SHOW_VLAN, 0, ch_sh_vlan, ACT_SHOW, "VLAN database"
};
SHOW_LEAF(n_sh_mac_at, "address-table", SHOW_MAC, "Learned and static addresses")
static __code const struct cli_node * __code const ch_sh_mac[] = {
	&n_sh_mac_at, 0
};
static __code const struct cli_node n_sh_mac = {
	"mac", 0, 0, 0, 0, ch_sh_mac, ACT_NONE, "MAC address table"
};
SHOW_LEAF(n_sh_stp, "spanning-tree", SHOW_STP, "Spanning tree state")
SHOW_LEAF(n_sh_tftp, "tftp", SHOW_TFTP, "State of the last TFTP transfer")
SHOW_LEAF(n_sh_hist, "history", SHOW_HIST, "Console command history")
SHOW_LEAF(n_sh_log, "logging", SHOW_LOG, "Remote syslog")
SHOW_LEAF(n_sh_igmp_snoop, "snooping", SHOW_IGMP, "IGMP snooping groups")
SHOW_LEAF(n_sh_po_sum, "summary", SHOW_PO, "Members and hash")
static __code const struct cli_node * __code const ch_sh_po[] = {
	&n_sh_po_sum, 0
};
static __code const struct cli_node n_sh_po = {
	"port-channel", 0, 0, SHOW_PO, 0, ch_sh_po, ACT_SHOW, "Port-channels"
};
SHOW_LEAF(n_sh_ip_if_brief, "brief", SHOW_IP_IF, "Management interface")
static __code const struct cli_node * __code const ch_sh_ip_if[] = {
	&n_sh_ip_if_brief, 0
};
static __code const struct cli_node n_sh_ip_if = {
	"interface", 0, 0, SHOW_IP_IF, 0, ch_sh_ip_if, ACT_SHOW, "IP interfaces"
};
static __code const struct cli_node * __code const ch_sh_igmp[] = {
	&n_sh_igmp_snoop, 0
};
static __code const struct cli_node n_sh_igmp = {
	"igmp", 0, 0, 0, 0, ch_sh_igmp, ACT_NONE, "IGMP"
};
static __code const struct cli_node * __code const ch_sh_ip[] = {
	&n_sh_igmp, &n_sh_ip_if, 0
};
static __code const struct cli_node n_sh_ip = {
	"ip", 0, 0, 0, 0, ch_sh_ip, ACT_NONE, "IP information"
};
/* NUM32 so that ->lo carries the show code: an ordinary NUM's lo is
 * its range, which would alias SHOW_IF_STATUS */
static __code const struct cli_node n_arg_sh_mon = {
	0, CLI_A_NUM32, 0, SHOW_MON, 0, NO_CHILDREN, ACT_SHOW, "Session number (1)"
};
static __code const struct cli_node * __code const ch_sh_mon_sess[] = {
	&n_arg_sh_mon, 0
};
static __code const struct cli_node n_sh_mon_sess = {
	"session", 0, 0, SHOW_MON, 0, ch_sh_mon_sess, ACT_SHOW, "SPAN session"
};
static __code const struct cli_node * __code const ch_sh_mon[] = {
	&n_sh_mon_sess, 0
};
static __code const struct cli_node n_sh_mon = {
	"monitor", 0, 0, SHOW_MON, 0, ch_sh_mon, ACT_SHOW, "Port mirroring"
};
static __code const struct cli_node * __code const ch_show[] = {
	&n_sh_hist, &n_sh_if, &n_sh_ip, &n_sh_log, &n_sh_mac, &n_sh_mon, &n_sh_po,
	&n_show_run, &n_sh_stp, &n_show_start, &n_sh_tftp, &n_show_version, &n_sh_vlan, 0
};

/* clear mac address-table dynamic */
static __code const struct cli_node n_clr_mac_dyn = {
	"dynamic", 0, CLI_F_PRIV, 0, 0, NO_CHILDREN, ACT_CLEAR_MAC, "Learned entries"
};
static __code const struct cli_node * __code const ch_clr_mac_at[] = {
	&n_clr_mac_dyn, 0
};
static __code const struct cli_node n_clr_mac_at = {
	"address-table", 0, CLI_F_PRIV, 0, 0, ch_clr_mac_at, ACT_NONE, "MAC address table"
};
static __code const struct cli_node * __code const ch_clr_mac[] = {
	&n_clr_mac_at, 0
};
static __code const struct cli_node n_clr_mac = {
	"mac", 0, CLI_F_PRIV, 0, 0, ch_clr_mac, ACT_NONE, "MAC address table"
};
static __code const struct cli_node * __code const ch_clear[] = {
	&n_clr_mac, 0
};
static __code const struct cli_node n_clear = {
	"clear", 0, CLI_F_PRIV, 0, 0, ch_clear, ACT_NONE, "Reset functions"
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
/* copy tftp flash|startup-config A.B.C.D FILE, copy startup-config tftp
 * A.B.C.D FILE; `config` is accepted for startup-config */
#define COPY_FILE(nm, op) \
static __code const struct cli_node nm##_file = { \
	0, CLI_A_WORD, CLI_F_PRIV, op, 0, NO_CHILDREN, ACT_COPY, "File name" \
}; \
static __code const struct cli_node * __code const nm##_filech[] = { &nm##_file, 0 }; \
static __code const struct cli_node nm##_ip = { \
	0, CLI_A_IP, CLI_F_PRIV, 0, 0, nm##_filech, ACT_NONE, "TFTP server address" \
}; \
static __code const struct cli_node * __code const nm##_ipch[] = { &nm##_ip, 0 };
COPY_FILE(n_cp_fw, TFTP_OP_GET_FW)
COPY_FILE(n_cp_cfgin, TFTP_OP_GET_CONFIG)
COPY_FILE(n_cp_cfgout, TFTP_OP_PUT_CONFIG)
static __code const struct cli_node n_cp_t_flash = {
	"flash", 0, CLI_F_PRIV, 0, 0, n_cp_fw_ipch, ACT_NONE, "Firmware image (applied at reboot)"
};
static __code const struct cli_node n_cp_t_start = {
	"startup-config", 0, CLI_F_PRIV, 0, 0, n_cp_cfgin_ipch, ACT_NONE, "Replace the startup config"
};
static __code const struct cli_node n_cp_t_cfg = {
	"config", 0, CLI_F_PRIV, 0, 0, n_cp_cfgin_ipch, ACT_NONE, "Same as startup-config"
};
static __code const struct cli_node * __code const ch_cp_tftp[] = {
	&n_cp_t_cfg, &n_cp_t_flash, &n_cp_t_start, 0
};
static __code const struct cli_node n_copy_tftp = {
	"tftp", 0, CLI_F_PRIV, 0, 0, ch_cp_tftp, ACT_NONE, "Download from a TFTP server"
};
static __code const struct cli_node n_cp_s_tftp = {
	"tftp", 0, CLI_F_PRIV, 0, 0, n_cp_cfgout_ipch, ACT_NONE, "Upload to a TFTP server"
};
static __code const struct cli_node * __code const ch_cp_start[] = {
	&n_cp_s_tftp, 0
};
static __code const struct cli_node n_copy_start = {
	"startup-config", 0, CLI_F_PRIV, 0, 0, ch_cp_start, ACT_NONE, "From the startup config"
};
static __code const struct cli_node n_copy_config = {
	"config", 0, CLI_F_PRIV, 0, 0, ch_cp_start, ACT_NONE, "Same as startup-config"
};
static __code const struct cli_node * __code const ch_copy[] = {
	&n_copy_config, &n_copy_running, &n_copy_start, &n_copy_tftp, 0
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

/* debug: raw chip access (privileged). The executing node is the last
 * argument, a HEX whose ->lo is free to carry the DBG_* operation. */
#define DBG_HEX(nm, op, help, kids) \
static __code const struct cli_node nm = { \
	0, CLI_A_HEX, CLI_F_PRIV, op, 0, kids, op ? ACT_DEBUG : ACT_NONE, help \
};
#define DBG_LEAF(nm, word, op, help) \
static __code const struct cli_node nm = { \
	word, 0, CLI_F_PRIV, op, 0, NO_CHILDREN, ACT_DEBUG, help \
};
#define DBG_KIDS(nm, ...) \
static __code const struct cli_node * __code const nm[] = { __VA_ARGS__, 0 };
#define DBG_WORD(nm, word, kids, help) \
static __code const struct cli_node nm = { \
	word, 0, CLI_F_PRIV, 0, 0, kids, ACT_NONE, help \
};
#define DBG_NUM(nm, hi, kids, help) \
static __code const struct cli_node nm = { \
	0, CLI_A_NUM, CLI_F_PRIV, 0, hi, kids, ACT_NONE, help \
};
/* register */
DBG_HEX(n_dr_rd_a, DBG_REG_RD, "Register address", NO_CHILDREN)
DBG_HEX(n_dr_wr_v, DBG_REG_WR, "32-bit value", NO_CHILDREN)
DBG_KIDS(ch_dr_wr_v, &n_dr_wr_v)
DBG_HEX(n_dr_wr_a, 0, "Register address", ch_dr_wr_v)
DBG_KIDS(ch_dr_rd, &n_dr_rd_a)
DBG_KIDS(ch_dr_wr, &n_dr_wr_a)
DBG_WORD(n_dr_read, "read", ch_dr_rd, "Read a switch register")
DBG_WORD(n_dr_write, "write", ch_dr_wr, "Write a switch register")
DBG_KIDS(ch_dreg, &n_dr_read, &n_dr_write)
DBG_WORD(n_d_reg, "register", ch_dreg, "Switch registers")
/* serdes <id> <page> <reg> [value] */
DBG_HEX(n_ds_rd_r, DBG_SDS_RD, "Register", NO_CHILDREN)
DBG_KIDS(ch_ds_rd_r, &n_ds_rd_r)
DBG_HEX(n_ds_rd_p, 0, "Page", ch_ds_rd_r)
DBG_KIDS(ch_ds_rd_p, &n_ds_rd_p)
DBG_NUM(n_ds_rd_id, 15, ch_ds_rd_p, "SerDes id")
DBG_HEX(n_ds_wr_v, DBG_SDS_WR, "16-bit value", NO_CHILDREN)
DBG_KIDS(ch_ds_wr_v, &n_ds_wr_v)
DBG_HEX(n_ds_wr_r, 0, "Register", ch_ds_wr_v)
DBG_KIDS(ch_ds_wr_r, &n_ds_wr_r)
DBG_HEX(n_ds_wr_p, 0, "Page", ch_ds_wr_r)
DBG_KIDS(ch_ds_wr_p, &n_ds_wr_p)
DBG_NUM(n_ds_wr_id, 15, ch_ds_wr_p, "SerDes id")
DBG_KIDS(ch_ds_rd, &n_ds_rd_id)
DBG_KIDS(ch_ds_wr, &n_ds_wr_id)
DBG_WORD(n_ds_read, "read", ch_ds_rd, "Read a SerDes register")
DBG_WORD(n_ds_write, "write", ch_ds_wr, "Write a SerDes register")
DBG_KIDS(ch_dsds, &n_ds_read, &n_ds_write)
DBG_WORD(n_d_sds, "serdes", ch_dsds, "SerDes registers")
/* phy <phy> <mmd> <reg> [value] */
DBG_HEX(n_dp_rd_r, DBG_PHY_RD, "Register", NO_CHILDREN)
DBG_KIDS(ch_dp_rd_r, &n_dp_rd_r)
DBG_NUM(n_dp_rd_d, 31, ch_dp_rd_r, "MMD device")
DBG_KIDS(ch_dp_rd_d, &n_dp_rd_d)
DBG_NUM(n_dp_rd_p, 31, ch_dp_rd_d, "PHY address")
DBG_HEX(n_dp_wr_v, DBG_PHY_WR, "16-bit value", NO_CHILDREN)
DBG_KIDS(ch_dp_wr_v, &n_dp_wr_v)
DBG_HEX(n_dp_wr_r, 0, "Register", ch_dp_wr_v)
DBG_KIDS(ch_dp_wr_r, &n_dp_wr_r)
DBG_NUM(n_dp_wr_d, 31, ch_dp_wr_r, "MMD device")
DBG_KIDS(ch_dp_wr_d, &n_dp_wr_d)
DBG_NUM(n_dp_wr_p, 31, ch_dp_wr_d, "PHY address")
DBG_KIDS(ch_dp_rd, &n_dp_rd_p)
DBG_KIDS(ch_dp_wr, &n_dp_wr_p)
DBG_WORD(n_dp_read, "read", ch_dp_rd, "Read a PHY register (clause 45)")
DBG_WORD(n_dp_write, "write", ch_dp_wr, "Write a PHY register (clause 45)")
DBG_KIDS(ch_dphy, &n_dp_read, &n_dp_write)
DBG_WORD(n_d_phy, "phy", ch_dphy, "PHY registers")
/* xram */
DBG_HEX(n_dx_rd_a, DBG_X_RD, "Address", NO_CHILDREN)
DBG_HEX(n_dx_t_n, DBG_X_TEST, "Length", NO_CHILDREN)
DBG_KIDS(ch_dx_t_n, &n_dx_t_n)
DBG_HEX(n_dx_t_a, 0, "Address (0x4000 or above)", ch_dx_t_n)
DBG_KIDS(ch_dx_rd, &n_dx_rd_a)
DBG_KIDS(ch_dx_t, &n_dx_t_a)
DBG_WORD(n_dx_read, "read", ch_dx_rd, "Dump 16 bytes")
DBG_WORD(n_dx_test, "test", ch_dx_t, "Destructive pattern test")
DBG_KIDS(ch_dxram, &n_dx_read, &n_dx_test)
DBG_WORD(n_d_xram, "xram", ch_dxram, "8051 external RAM")
/* flash */
DBG_LEAF(n_df_id, "id", DBG_FL_ID, "JEDEC id")
DBG_LEAF(n_df_uid, "uid", DBG_FL_UID, "Unique id")
DBG_LEAF(n_df_sec, "security", DBG_FL_SEC, "Security registers")
DBG_KIDS(ch_dflash, &n_df_id, &n_df_sec, &n_df_uid)
DBG_WORD(n_d_flash, "flash", ch_dflash, "SPI flash")
DBG_LEAF(n_d_gpio, "gpio", DBG_GPIO, "GPIO inputs and changes")
DBG_LEAF(n_d_rnd, "random", DBG_RND, "Hardware random number")
DBG_KIDS(ch_debug, &n_d_flash, &n_d_gpio, &n_d_phy, &n_d_rnd, &n_d_reg, &n_d_sds, &n_d_xram)
static __code const struct cli_node n_debug = {
	"debug", 0, CLI_F_PRIV, 0, 0, ch_debug, ACT_NONE, "Raw hardware access"
};

static __code const struct cli_node * __code const cli_root_exec[] = {
	&n_clear, &n_configure, &n_copy, &n_debug, &n_disable, &n_enable,
	&n_exit_exec, &n_reload, &n_show, &n_write, 0
};

/* ---- global configuration mode ---- */

/* interface ethernet S/N | interface vlan N | interface S/N */
static __code const struct cli_node n_arg_ifnum = {
	0, CLI_A_IFLIST, 0, 0, 0, NO_CHILDREN, ACT_IF,
	"Interface number or list (e.g. 1/5, 1/1-4,1/7)"
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
static __code const struct cli_node n_arg_po = {
	0, CLI_A_NUM, 0, 1, 4, NO_CHILDREN, ACT_PO, "Port-channel number"
};
static __code const struct cli_node * __code const ch_ifpo[] = {
	&n_arg_po, 0
};
static __code const struct cli_node n_if_po = {
	"port-channel", 0, 0, 0, 0, ch_ifpo, ACT_NONE, "Link aggregation group"
};
/* interface range ethernet 1/1-4,1/7: the same list `interface ethernet`
 * takes, spelled the IOS way */
static __code const struct cli_node * __code const ch_ifrange[] = {
	&n_if_ethernet, &n_arg_ifnum, 0
};
static __code const struct cli_node n_if_range = {
	"range", 0, 0, 0, 0, ch_ifrange, ACT_NONE,
	"Configure several ethernet interfaces at once"
};
static __code const struct cli_node * __code const ch_interface[] = {
	&n_if_ethernet, &n_if_po, &n_if_range, &n_if_vlan, &n_arg_ifnum, 0
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

/* monitor session 1 source|destination interface ethernet S/N */
static __code const struct cli_node n_mon_rx = {
	"rx", 0, 0, 1, 0, NO_CHILDREN, ACT_MON_SRC, "Received traffic"
};
static __code const struct cli_node n_mon_tx = {
	"tx", 0, 0, 2, 0, NO_CHILDREN, ACT_MON_SRC, "Transmitted traffic"
};
static __code const struct cli_node n_mon_both = {
	"both", 0, 0, 3, 0, NO_CHILDREN, ACT_MON_SRC, "Both directions (default)"
};
static __code const struct cli_node * __code const ch_mon_dir[] = {
	&n_mon_both, &n_mon_rx, &n_mon_tx, 0
};
/* the IFACE placeholder ignores lo/hi, so ->lo carries the default direction */
static __code const struct cli_node n_arg_mon_src = {
	0, CLI_A_IFACE, CLI_F_NO_OK, 3, 0, ch_mon_dir, ACT_MON_SRC, "Source interface"
};
static __code const struct cli_node * __code const ch_mon_src_eth[] = {
	&n_arg_mon_src, 0
};
static __code const struct cli_node n_mon_src_eth = {
	"ethernet", 0, 0, 0, 0, ch_mon_src_eth, ACT_NONE, "Ethernet interface"
};
static __code const struct cli_node * __code const ch_mon_src_if[] = {
	&n_mon_src_eth, &n_arg_mon_src, 0
};
static __code const struct cli_node n_mon_src_if = {
	"interface", 0, 0, 0, 0, ch_mon_src_if, ACT_NONE, "Source interface"
};
static __code const struct cli_node * __code const ch_mon_source[] = {
	&n_mon_src_if, 0
};
static __code const struct cli_node n_mon_source = {
	"source", 0, 0, 0, 0, ch_mon_source, ACT_NONE, "Traffic to copy"
};
static __code const struct cli_node n_arg_mon_dst = {
	0, CLI_A_IFACE, CLI_F_NO_OK, 0, 0, NO_CHILDREN, ACT_MON_DST, "Destination interface"
};
static __code const struct cli_node * __code const ch_mon_dst_eth[] = {
	&n_arg_mon_dst, 0
};
static __code const struct cli_node n_mon_dst_eth = {
	"ethernet", 0, 0, 0, 0, ch_mon_dst_eth, ACT_NONE, "Ethernet interface"
};
static __code const struct cli_node * __code const ch_mon_dst_if[] = {
	&n_mon_dst_eth, &n_arg_mon_dst, 0
};
static __code const struct cli_node n_mon_dst_if = {
	"interface", 0, 0, 0, 0, ch_mon_dst_if, ACT_NONE, "Destination interface"
};
static __code const struct cli_node * __code const ch_mon_dest[] = {
	&n_mon_dst_if, 0
};
static __code const struct cli_node n_mon_dest = {
	"destination", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, ch_mon_dest, ACT_MON_DST,
	"Where the copies go"
};
static __code const struct cli_node * __code const ch_mon_sess[] = {
	&n_mon_dest, &n_mon_source, 0
};
static __code const struct cli_node n_arg_mon_sess = {
	0, CLI_A_NUM, CLI_F_NO_OK | CLI_F_NO_EXEC, 1, 1, ch_mon_sess, ACT_MON_DEL,
	"Session number"
};
static __code const struct cli_node * __code const ch_mon_session[] = {
	&n_arg_mon_sess, 0
};
static __code const struct cli_node n_mon_session = {
	"session", 0, 0, 0, 0, ch_mon_session, ACT_NONE, "SPAN session"
};
static __code const struct cli_node * __code const ch_monitor[] = {
	&n_mon_session, 0
};
static __code const struct cli_node n_monitor = {
	"monitor", 0, 0, 0, 0, ch_monitor, ACT_NONE, "Port mirroring"
};

/* global spanning-tree; the numeric arguments are NUM32 so ->lo can
 * carry the parameter, the handler range-checks */
static __code const struct cli_node n_stpg_rstp = {
	"rstp", 0, 0, STPG_RSTP, 0, NO_CHILDREN, ACT_STP_G, "Rapid spanning tree (802.1w)"
};
static __code const struct cli_node n_stpg_stp = {
	"stp", 0, 0, STPG_STP, 0, NO_CHILDREN, ACT_STP_G, "Classic spanning tree (802.1D)"
};
static __code const struct cli_node * __code const ch_stpg_mode[] = {
	&n_stpg_rstp, &n_stpg_stp, 0
};
static __code const struct cli_node n_stpg_mode = {
	"mode", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, STPG_RSTP, 0, ch_stpg_mode, ACT_STP_G,
	"Protocol version"
};
#define STPG_NUM(nm, code, help) \
static __code const struct cli_node nm##_arg = { \
	0, CLI_A_NUM32, 0, code, 0, NO_CHILDREN, ACT_STP_G, help \
}; \
static __code const struct cli_node * __code const nm##_ch[] = { &nm##_arg, 0 };
STPG_NUM(n_stpg_prio, STPG_PRIO, "0-61440, a multiple of 4096")
STPG_NUM(n_stpg_hello, STPG_HELLO, "Seconds, 1-10")
STPG_NUM(n_stpg_fwd, STPG_FWD, "Seconds, 4-30")
STPG_NUM(n_stpg_maxage, STPG_MAXAGE, "Seconds, 6-40")
STPG_NUM(n_stpg_hold, STPG_TXHOLD, "BPDUs per second, 1-10")
static __code const struct cli_node n_stpg_priority = {
	"priority", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, STPG_PRIO, 0, n_stpg_prio_ch, ACT_STP_G,
	"Bridge priority"
};
static __code const struct cli_node n_stpg_hellot = {
	"hello-time", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, STPG_HELLO, 0, n_stpg_hello_ch, ACT_STP_G,
	"BPDU interval"
};
static __code const struct cli_node n_stpg_fwdt = {
	"forward-time", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, STPG_FWD, 0, n_stpg_fwd_ch, ACT_STP_G,
	"Forward delay"
};
static __code const struct cli_node n_stpg_maxaget = {
	"max-age", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, STPG_MAXAGE, 0, n_stpg_maxage_ch, ACT_STP_G,
	"Maximum message age"
};
static __code const struct cli_node n_stpg_holdcnt = {
	"hold-count", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, STPG_TXHOLD, 0, n_stpg_hold_ch, ACT_STP_G,
	"Transmit limit"
};
static __code const struct cli_node * __code const ch_stpg_tx[] = {
	&n_stpg_holdcnt, 0
};
static __code const struct cli_node n_stpg_transmit = {
	"transmit", 0, 0, 0, 0, ch_stpg_tx, ACT_NONE, "BPDU transmission"
};
static __code const struct cli_node * __code const ch_stp_global[] = {
	&n_stpg_fwdt, &n_stpg_hellot, &n_stpg_maxaget, &n_stpg_mode,
	&n_stpg_priority, &n_stpg_transmit, 0
};
static __code const struct cli_node n_stp_global = {
	"spanning-tree", 0, 0, 0, 0, ch_stp_global, ACT_NONE, "Spanning tree bridge settings"
};

/* feature telnet (NX-OS style service toggle) */
static __code const struct cli_node n_feat_telnet = {
	"telnet", 0, CLI_F_NO_OK, 0, 0, NO_CHILDREN, ACT_FEAT_TELNET,
	"Telnet server"
};
static __code const struct cli_node n_feat_stp = {
	"spanning-tree", 0, CLI_F_NO_OK, 0, 0, NO_CHILDREN, ACT_FEAT_STP,
	"Spanning tree protocol"
};
static __code const struct cli_node * __code const ch_feature[] = {
	&n_feat_stp, &n_feat_telnet, 0
};
static __code const struct cli_node n_feature = {
	"feature", 0, 0, 0, 0, ch_feature, ACT_NONE,
	"Enable or disable a service"
};

/* line vty [first [last]] - the numbers are accepted and ignored: there
 * is one telnet session */
static __code const struct cli_node n_arg_vty_last = {
	0, CLI_A_NUM, 0, 0, 15, NO_CHILDREN, ACT_LINE_VTY, "Last line number"
};
static __code const struct cli_node * __code const ch_vty_first[] = {
	&n_arg_vty_last, 0
};
static __code const struct cli_node n_arg_vty_first = {
	0, CLI_A_NUM, 0, 0, 15, ch_vty_first, ACT_LINE_VTY, "First line number"
};
static __code const struct cli_node * __code const ch_vty[] = {
	&n_arg_vty_first, 0
};
static __code const struct cli_node n_line_vty = {
	"vty", 0, 0, 0, 0, ch_vty, ACT_LINE_VTY, "Telnet session"
};
static __code const struct cli_node * __code const ch_line[] = {
	&n_line_vty, 0
};
static __code const struct cli_node n_line = {
	"line", 0, 0, 0, 0, ch_line, ACT_NONE, "Configure a terminal line"
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
	&n_end, &n_exit_cfg, &n_feature, &n_hostname, &n_interface, &n_ip_cfg,
	&n_line, &n_logging, &n_monitor, &n_stp_global, &n_vlan, 0
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
	"speed", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, PHY_SPEED_AUTO, 0, ch_speed, ACT_SPEED,
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
	"mtu", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, ch_mtu, ACT_MTU,
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
static __code const struct cli_node n_sw_protected = {
	"protected", 0, CLI_F_NO_OK, 0, 0, NO_CHILDREN, ACT_PROT,
	"No forwarding to other protected ports"
};
static __code const struct cli_node * __code const ch_switchport[] = {
	&n_sw_access, &n_sw_mode, &n_sw_protected, &n_sw_trunk, 0
};
static __code const struct cli_node n_if_switchport = {
	"switchport", 0, 0, 0, 0, ch_switchport, ACT_NONE,
	"Set switching mode characteristics"
};

/* power efficient-ethernet auto */
static __code const struct cli_node n_eee_auto = {
	"auto", 0, 0, 0, 0, NO_CHILDREN, ACT_EEE, "Negotiate EEE (default)"
};
static __code const struct cli_node * __code const ch_pw_eee[] = {
	&n_eee_auto, 0
};
static __code const struct cli_node n_pw_eee = {
	"efficient-ethernet", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, ch_pw_eee, ACT_EEE,
	"Energy Efficient Ethernet (802.3az)"
};
static __code const struct cli_node * __code const ch_power[] = {
	&n_pw_eee, 0
};
static __code const struct cli_node n_if_power = {
	"power", 0, 0, 0, 0, ch_power, ACT_NONE, "Power saving"
};

/* rate-limit input|output KBPS [drop] */
static __code const struct cli_node n_rl_drop = {
	"drop", 0, 0, 3, 0, NO_CHILDREN, ACT_RL, "Drop instead of sending pause frames"
};
static __code const struct cli_node * __code const ch_rl_in_arg[] = {
	&n_rl_drop, 0
};
static __code const struct cli_node n_arg_rl_in = {
	0, CLI_A_NUM32, 0, 1, 0, ch_rl_in_arg, ACT_RL, "Kbit/s, 16-10000000 in steps of 16"
};
static __code const struct cli_node n_arg_rl_out = {
	0, CLI_A_NUM32, 0, 2, 0, NO_CHILDREN, ACT_RL, "Kbit/s, 16-10000000 in steps of 16"
};
static __code const struct cli_node * __code const ch_rl_in[] = {
	&n_arg_rl_in, 0
};
static __code const struct cli_node * __code const ch_rl_out[] = {
	&n_arg_rl_out, 0
};
static __code const struct cli_node n_rl_input = {
	"input", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 1, 0, ch_rl_in, ACT_RL, "Ingress limit"
};
static __code const struct cli_node n_rl_output = {
	"output", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 2, 0, ch_rl_out, ACT_RL, "Egress limit"
};
static __code const struct cli_node * __code const ch_rl[] = {
	&n_rl_input, &n_rl_output, 0
};
static __code const struct cli_node n_if_rl = {
	"rate-limit", 0, 0, 0, 0, ch_rl, ACT_NONE, "Bandwidth limit"
};

/* channel-group N [mode on] (static aggregation; there is no LACP) */
static __code const struct cli_node n_cg_on = {
	"on", 0, 0, 0, 0, NO_CHILDREN, ACT_CHGRP, "Static aggregation"
};
static __code const struct cli_node * __code const ch_cg_mode[] = {
	&n_cg_on, 0
};
static __code const struct cli_node n_cg_mode = {
	"mode", 0, 0, 0, 0, ch_cg_mode, ACT_NONE, "Aggregation mode"
};
static __code const struct cli_node * __code const ch_cg_arg[] = {
	&n_cg_mode, 0
};
static __code const struct cli_node n_arg_cg = {
	0, CLI_A_NUM, 0, 1, 4, ch_cg_arg, ACT_CHGRP, "Port-channel number"
};
static __code const struct cli_node * __code const ch_cg[] = {
	&n_arg_cg, 0
};
static __code const struct cli_node n_if_cg = {
	"channel-group", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, ch_cg, ACT_CHGRP,
	"Add the port to a port-channel"
};

/* per-port / per-port-channel spanning-tree */
static __code const struct cli_node n_stpi_pf_dis = {
	"disable", 0, 0, STPI_PF_DIS, 0, NO_CHILDREN, ACT_STP_IF, "Neither admin nor auto edge"
};
static __code const struct cli_node * __code const ch_stpi_pf[] = {
	&n_stpi_pf_dis, 0
};
static __code const struct cli_node n_stpi_portfast = {
	"portfast", 0, CLI_F_NO_OK, STPI_PORTFAST, 0, ch_stpi_pf, ACT_STP_IF,
	"Edge port: forward immediately"
};
static __code const struct cli_node n_stpi_bg_en = {
	"enable", 0, 0, STPI_BPDUGUARD, 0, NO_CHILDREN, ACT_STP_IF, "Shut the port on a BPDU"
};
static __code const struct cli_node * __code const ch_stpi_bg[] = {
	&n_stpi_bg_en, 0
};
static __code const struct cli_node n_stpi_bguard = {
	"bpduguard", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, STPI_BPDUGUARD, 0, ch_stpi_bg, ACT_STP_IF,
	"BPDU guard"
};
static __code const struct cli_node n_stpi_bf_en = {
	"enable", 0, 0, STPI_BPDUFILT, 0, NO_CHILDREN, ACT_STP_IF, "Neither send nor accept BPDUs"
};
static __code const struct cli_node * __code const ch_stpi_bf[] = {
	&n_stpi_bf_en, 0
};
static __code const struct cli_node n_stpi_bfilter = {
	"bpdufilter", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, STPI_BPDUFILT, 0, ch_stpi_bf, ACT_STP_IF,
	"BPDU filter"
};
static __code const struct cli_node n_stpi_g_root = {
	"root", 0, 0, STPI_ROOTGUARD, 0, NO_CHILDREN, ACT_STP_IF, "Never accept a better root here"
};
static __code const struct cli_node * __code const ch_stpi_guard[] = {
	&n_stpi_g_root, 0
};
static __code const struct cli_node n_stpi_guard = {
	"guard", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, STPI_ROOTGUARD, 0, ch_stpi_guard, ACT_STP_IF,
	"Root guard"
};
static __code const struct cli_node n_arg_stpi_cost = {
	0, CLI_A_NUM32, 0, STPI_COST, 0, NO_CHILDREN, ACT_STP_IF, "1-200000000"
};
static __code const struct cli_node * __code const ch_stpi_cost[] = {
	&n_arg_stpi_cost, 0
};
static __code const struct cli_node n_stpi_cost = {
	"cost", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, STPI_COST, 0, ch_stpi_cost, ACT_STP_IF,
	"Path cost (default: by speed)"
};
static __code const struct cli_node n_arg_stpi_pp = {
	0, CLI_A_NUM32, 0, STPI_PPRIO, 0, NO_CHILDREN, ACT_STP_IF, "0-240, a multiple of 16"
};
static __code const struct cli_node * __code const ch_stpi_pp[] = {
	&n_arg_stpi_pp, 0
};
static __code const struct cli_node n_stpi_pprio = {
	"port-priority", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, STPI_PPRIO, 0, ch_stpi_pp, ACT_STP_IF,
	"Port priority"
};
static __code const struct cli_node n_stpi_p2p = {
	"point-to-point", 0, 0, STPI_P2P, 0, NO_CHILDREN, ACT_STP_IF, "Full-duplex link"
};
static __code const struct cli_node n_stpi_shared = {
	"shared", 0, 0, STPI_SHARED, 0, NO_CHILDREN, ACT_STP_IF, "Shared medium"
};
static __code const struct cli_node * __code const ch_stpi_lt[] = {
	&n_stpi_p2p, &n_stpi_shared, 0
};
static __code const struct cli_node n_stpi_lt = {
	"link-type", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, STPI_P2P, 0, ch_stpi_lt, ACT_STP_IF,
	"Link type (default: auto)"
};
static __code const struct cli_node * __code const ch_stp_if[] = {
	&n_stpi_bfilter, &n_stpi_bguard, &n_stpi_cost, &n_stpi_guard,
	&n_stpi_lt, &n_stpi_pprio, &n_stpi_portfast, 0
};
static __code const struct cli_node n_if_stp = {
	"spanning-tree", 0, 0, 0, 0, ch_stp_if, ACT_NONE, "Spanning tree port settings"
};

static __code const struct cli_node n_dx_auto = {
	"auto", 0, 0, PHY_DUPLEX_BOTH, 0, NO_CHILDREN, ACT_DUPLEX, "Negotiate (default)"
};
static __code const struct cli_node n_dx_full = {
	"full", 0, 0, PHY_DUPLEX_FULL, 0, NO_CHILDREN, ACT_DUPLEX, "Full duplex"
};
static __code const struct cli_node n_dx_half = {
	"half", 0, 0, PHY_DUPLEX_HALF, 0, NO_CHILDREN, ACT_DUPLEX, "Half duplex (10/100 only)"
};
static __code const struct cli_node * __code const ch_duplex[] = {
	&n_dx_auto, &n_dx_full, &n_dx_half, 0
};
static __code const struct cli_node n_if_duplex = {
	"duplex", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, PHY_DUPLEX_BOTH, 0, ch_duplex, ACT_DUPLEX,
	"Set the duplex mode"
};
static __code const struct cli_node * __code const cli_root_if[] = {
	&n_end, &n_exit_cfg, &n_if_cg, &n_if_description, &n_if_duplex, &n_if_mtu,
	&n_if_power, &n_if_rl, &n_if_shutdown, &n_if_speed, &n_if_stp, &n_if_switchport, 0
};

/* ---- port-channel mode ---- */
/* load-balance FIELD...: the fields chain back into the same list and
 * each ORs its hash bit into cli.acc */
extern __code const struct cli_node * __code const cli_ch_lb[];
#define LB_FIELD(nm, word, bit, help) \
static __code const struct cli_node nm = { \
	word, 0, CLI_F_ACC, bit, 0, cli_ch_lb, ACT_LB, help \
};
LB_FIELD(n_lb_sport, "src-port", LAG_HASH_SOURCE_PORT_NUMBER, "Ingress port")
LB_FIELD(n_lb_smac, "src-mac", LAG_HASH_L2_SMAC, "Source MAC")
LB_FIELD(n_lb_dmac, "dst-mac", LAG_HASH_L2_DMAC, "Destination MAC")
LB_FIELD(n_lb_sip, "src-ip", LAG_HASH_L3_SIP, "Source IP")
LB_FIELD(n_lb_dip, "dst-ip", LAG_HASH_L3_DIP, "Destination IP")
LB_FIELD(n_lb_l4s, "l4-src-port", LAG_HASH_L4_SPORT, "TCP/UDP source port")
LB_FIELD(n_lb_l4d, "l4-dst-port", LAG_HASH_L4_DPORT, "TCP/UDP destination port")
__code const struct cli_node * __code const cli_ch_lb[] = {
	&n_lb_dip, &n_lb_dmac, &n_lb_l4d, &n_lb_l4s, &n_lb_sip, &n_lb_smac, &n_lb_sport, 0
};
static __code const struct cli_node n_po_lb = {
	"load-balance", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, cli_ch_lb, ACT_LB,
	"Hash fields for member selection"
};
static __code const struct cli_node * __code const cli_root_po[] = {
	&n_end, &n_exit_cfg, &n_po_lb, &n_if_stp, 0
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
static __code const struct cli_node n_arg_svi_mac = {
	0, CLI_A_WORD, 0, 0, 0, NO_CHILDREN, ACT_MACADDR, "aabb.ccdd.eeff or aa:bb:cc:dd:ee:ff"
};
static __code const struct cli_node * __code const ch_svi_mac[] = {
	&n_arg_svi_mac, 0
};
static __code const struct cli_node n_svi_mac = {
	"mac-address", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, ch_svi_mac, ACT_MACADDR,
	"Management MAC address"
};
static __code const struct cli_node * __code const cli_root_svi[] = {
	&n_end, &n_exit_cfg, &n_svi_ip, &n_svi_mac, 0
};

/* ---- line configuration mode ---- */
static __code const struct cli_node n_arg_to_sec = {
	0, CLI_A_NUM, 0, 0, 59, NO_CHILDREN, ACT_EXEC_TO, "Seconds"
};
static __code const struct cli_node * __code const ch_to_min[] = {
	&n_arg_to_sec, 0
};
static __code const struct cli_node n_arg_to_min = {
	0, CLI_A_NUM, 0, 0, 1091, ch_to_min, ACT_EXEC_TO, "Minutes (0 0 = never)"
};
static __code const struct cli_node * __code const ch_exec_to[] = {
	&n_arg_to_min, 0
};
static __code const struct cli_node n_ln_exec_to = {
	"exec-timeout", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, ch_exec_to, ACT_EXEC_TO,
	"Idle timeout of the session"
};
static __code const struct cli_node n_arg_vty_pw = {
	0, CLI_A_WORD, 0, 0, 0, NO_CHILDREN, ACT_VTY_PW, "Up to 20 characters"
};
static __code const struct cli_node * __code const ch_vty_pw[] = {
	&n_arg_vty_pw, 0
};
static __code const struct cli_node n_ln_password = {
	"password", 0, CLI_F_NO_OK | CLI_F_NO_EXEC, 0, 0, ch_vty_pw, ACT_VTY_PW,
	"Login password"
};
static __code const struct cli_node * __code const cli_root_line[] = {
	&n_end, &n_exit_cfg, &n_ln_exec_to, &n_ln_password, 0
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
	case CLI_MODE_PO:
		return cli_root_po;
	case CLI_MODE_LINE:
		return cli_root_line;
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


/* Argument parsers for arg_accept(): leaves with their state in xdata,
 * because every 32-bit temporary held across a call in arg_accept() costs
 * internal RAM the overlay segment does not have. They read the token at
 * ap_p/ap_len and leave the value in ap_v. */
static __xdata char * __xdata ap_p;
static __xdata uint8_t ap_len, ap_i;
static __xdata uint32_t ap_v;

/* decimal, up to maxlen digits */
static uint8_t ap_dec(uint8_t maxlen)
{
	if (!ap_len || ap_len > maxlen)
		return 0;
	ap_v = 0;
	for (ap_i = 0; ap_i < ap_len; ap_i++) {
		if (ap_p[ap_i] < '0' || ap_p[ap_i] > '9')
			return 0;
		ap_v = ap_v * 10 + (ap_p[ap_i] - '0');
	}
	return 1;
}

static uint8_t ap_ip(void)
{
	static __xdata uint8_t dots, digits;
	static __xdata uint16_t oct;

	dots = 0; digits = 0; oct = 0;
	ap_v = 0;
	for (ap_i = 0; ap_i < ap_len; ap_i++) {
		if (ap_p[ap_i] == '.') {
			if (!digits || dots == 3)
				return 0;
			ap_v = (ap_v << 8) | oct;
			oct = 0;
			digits = 0;
			dots++;
		} else if (ap_p[ap_i] >= '0' && ap_p[ap_i] <= '9') {
			oct = oct * 10 + (ap_p[ap_i] - '0');
			if (oct > 255 || ++digits > 3)
				return 0;
		} else {
			return 0;
		}
	}
	if (dots != 3 || !digits)
		return 0;
	ap_v = (ap_v << 8) | oct;
	return 1;
}

/* hexadecimal up to 8 digits, optional 0x */
static uint8_t ap_hex(void)
{
	static __xdata char c;

	ap_i = 0;
	if (ap_len > 2 && ap_p[0] == '0' && (ap_p[1] == 'x' || ap_p[1] == 'X'))
		ap_i = 2;
	if (ap_len - ap_i < 1 || ap_len - ap_i > 8)
		return 0;
	ap_v = 0;
	for (; ap_i < ap_len; ap_i++) {
		c = ap_p[ap_i] | 0x20;
		ap_v <<= 4;
		if (c >= '0' && c <= '9')
			ap_v |= c - '0';
		else if (c >= 'a' && c <= 'f')
			ap_v |= c - 'a' + 10;
		else
			return 0;
	}
	return 1;
}

/* Interface token: item[,item...], item = [ethernet-prefix][S/]A[-[S/]B];
 * the port is the number after the last '/'. With one set only a single
 * port is accepted and ap_v is N, else ap_v is the ports as a mask
 * (bit N). */
static uint8_t ap_iface(uint8_t one)
{
	static __code const char * __xdata w;
	static __xdata uint16_t num, first, mask;
	static __xdata uint8_t have, dash, single;
	static __xdata char c;

	single = one;
	ap_i = 0;
	dash = 0;
	first = 0;
	mask = 0;
	for (;;) {
		w = "ethernet";
		num = 0;
		have = 0;
		while (ap_i < ap_len) {
			c = ap_p[ap_i] | 0x20;
			if (c < 'a' || c > 'z')
				break;
			if (!*w || c != *w)
				return 0;
			w++;
			ap_i++;
		}
		c = 0;
		for (; ap_i < ap_len; ap_i++) {
			c = ap_p[ap_i];
			if (c == ',' || c == '-')
				break;
			if (c == '/') {
				if (!have)
					return 0;
				num = 0;
				have = 0;
			} else if (c >= '0' && c <= '9') {
				num = num * 10 + (c - '0');
				have = 1;
				if (num > 99)
					return 0;
			} else {
				return 0;
			}
		}
		if (!have || num < 1 || num > 9)
			return 0;
		if (single) {
			ap_v = num;
			return ap_i == ap_len;
		}
		if (!dash)
			first = num;
		else if (num < first)
			return 0;
		if (ap_i < ap_len && c == '-') {
			if (dash)
				return 0;
			dash = 1;
			ap_i++;
			continue;
		}
		for (; first <= num; first++)
			mask |= 1 << first;
		if (ap_i == ap_len) {
			ap_v = mask;
			return 1;
		}
		dash = 0;
		ap_i++;	/* ',' */
	}
}


/* Validate token t against an argument node; store the value. Returns 0
 * on mismatch. */
static uint8_t arg_accept(uint8_t t, __code const struct cli_node *n)
{
	if (cli.nargs >= CLI_MAX_ARGS)
		return 0;

	ap_p = cli_line + tok_off[t];
	ap_len = tok_len[t];
	ap_v = 0;
	switch (n->arg) {
	case CLI_A_NUM:
		if (!ap_dec(5) || ap_v < n->lo || ap_v > n->hi)
			return 0;
		break;
	case CLI_A_NUM32:
		if (!ap_dec(9))
			return 0;
		break;
	case CLI_A_IP:
		if (!ap_ip())
			return 0;
		break;
	case CLI_A_IFACE:
	case CLI_A_IFLIST:
		if (!ap_iface(n->arg == CLI_A_IFACE))
			return 0;
		break;
	case CLI_A_HEX:
		if (!ap_hex())
			return 0;
		break;
	case CLI_A_WORD:
		if (!ap_len)
			return 0;
		break;
	case CLI_A_LINE:
		break;
	default:
		return 0;
	}

	cli.args[cli.nargs] = ap_v;
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
	cli.acc = 0;

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
		if (sel->flags & CLI_F_ACC)
			cli.acc |= sel->lo;
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
	if (cli_replaying) {
		/* no prompt was printed: show the startup-config line itself */
		print_string("% In the startup config:\n");
		print_string_x(cli_line);
		write_char('\n');
		cli_spaces(tok_off[w_badtok]);
		print_string("^\n% Invalid input detected at '^' marker.\n\n");
		return;
	}
	cli_spaces(cli.plen + tok_off[w_badtok]);
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
	case CLI_A_IFLIST:
		print_string("S/N[-N][,S/N...]");
		break;
	case CLI_A_NUM32:
		print_string("<number>");
		break;
	case CLI_A_HEX:
		print_string("<hex>");
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




/* ---------------- public entry points ---------------- */

#define CLI_SESSION_BYTES offsetof(struct cli_state_t, no)
static __xdata uint8_t cli_saved[2][CLI_SESSION_BYTES];
static __xdata uint8_t cli_who;

void cli_use(uint8_t who) __banked
{
	static __xdata uint8_t i, w;
	static __xdata uint8_t * __xdata c;

	w = who;
	if (w == cli_who)
		return;
	c = (__xdata uint8_t *)&cli;
	for (i = 0; i < CLI_SESSION_BYTES; i++) {
		cli_saved[cli_who][i] = c[i];
		c[i] = cli_saved[w][i];
	}
	cli_who = w;
}


void cli_init(void) __banked
{
	static __xdata uint8_t i;

	cli.mode = CLI_MODE_EXEC;
	cli.await = CLI_AWAIT_NONE;
	cli.no = 0;
	cli.ctx_if = cli.ctx_lport = cli.ctx_line = cli.ctx_po = 0;
	cli.ctx_vlan = 0;
	cli.ctx_range = 0;
	for (i = 0; i < CLI_SESSION_BYTES; i++) {
		cli_saved[CLI_CONSOLE][i] = ((__xdata uint8_t *)&cli)[i];
		cli_saved[CLI_VTY][i] = ((__xdata uint8_t *)&cli)[i];
	}
	cli_who = CLI_CONSOLE;
	cli_replaying = 0;
	dedupe_root = 0;
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
	if (line[tok_off[0]] == '!')
		return;		/* comment, as in a pasted or saved config */

	cli_strip_no();
	if (cli.no && !ntok) {
		print_string("% Incomplete command.\n\n");
		return;
	}

	cli_walk_roots(ntok);

	switch (w_status) {
	case W_NOMATCH0:
		cli_marker_error();
		return;
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
	cli.lo = w_node->lo;
	cli.line = cli_line;
	if (cli.mode == CLI_MODE_IF && cli.ctx_range && w_level == WL_MODE)
		cli_act_range(w_node->action);
	else
		cli_act(w_node->action);
}


void cli_replay_begin(void) __banked
{
	cli_use(CLI_CONSOLE);
	cli.mode = CLI_MODE_CONFIG;
	cli_replaying = 1;
	sw_defer(1);
}


void cli_replay_line(__xdata char *line) __banked
{
	cli_exec_line(line);
}


void cli_replay_end(void) __banked
{
	cli_replaying = 0;
	cli.mode = CLI_MODE_EXEC;
	sw_defer(0);
}


/* No leading newline: callers position the cursor (fresh line after a
 * command, or an in-place redraw from the line editor). */
void cli_prompt(void) __banked
{
	__xdata char *h = hostname;

	cli.plen = 0;
	while (*h) {
		write_char(*h++);
		cli.plen++;
	}
	switch (cli.mode) {
	case CLI_MODE_EXEC:
		print_string("> ");
		cli.plen += 2;
		return;
	case CLI_MODE_CONFIG:
		print_string("(config)");
		cli.plen += 8;
		break;
	case CLI_MODE_IF:
		if (cli.ctx_range) {
			print_string("(config-if-range)");
			cli.plen += 17;
			break;
		}
		/* fall through */
	case CLI_MODE_SVI:
	case CLI_MODE_PO:
		print_string("(config-if)");
		cli.plen += 11;
		break;
	case CLI_MODE_VLAN:
		print_string("(config-vlan)");
		cli.plen += 13;
		break;
	case CLI_MODE_LINE:
		print_string("(config-line)");
		cli.plen += 13;
		break;
	}
	print_string("# ");
	cli.plen += 2;
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
