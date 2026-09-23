/*
 * test_cli.c - host unit tests for cli.c (the modal CLI engine).
 *
 * Drives cli_exec_line/cli_help/cli_complete directly with fabricated
 * lines and asserts on mode transitions, abbreviation, ambiguity and
 * error output, rejection of the old flat syntax, and the NX-OS-style exec-anywhere
 * behavior.
 */
#include "sdcc_shim.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "rtl837x_common.h"
#include "cli.h"
#include "support.h"

#include "rtl837x_phy.h"
#include "rtl837x_port.h"
#include "swcfg.h"
#include "uip.h"
#include "syslog.h"
#include "hw_mock.h"
#include "runcfg.h"
#include "telnetd.h"
#include "dhcp.h"
#include "lacp.h"
#include "dns.h"
#include "ntp.h"
#include "totp.h"
#include "log.h"
#include "lldp.h"
#include "rtl837x_regs.h"
#include "rtl837x_stp.h"
#include "tftp.h"
#include "phy.h"
extern int n_reset, n_setspeed;
extern uint8_t last_speed, last_port;
extern char port_names[9][PORT_NAME_SIZE];
void env_cli_reset(void);

extern uint8_t last_tftp_op, last_tftp_srv[4];
extern char last_tftp_file[64];
extern uint8_t last_sds_id, last_sds_page, last_sds_reg;
extern uint16_t last_sds_val;
extern int n_igmp_show;

static char linebuf[CMD_BUF_SIZE];

static void run(const char *s)
{
	strncpy(linebuf, s, sizeof(linebuf) - 1);
	linebuf[sizeof(linebuf) - 1] = 0;
	out_reset();
	cli_exec_line(linebuf);
}

extern char hostname[24];
extern uint16_t management_vlan;
extern uint16_t vlan_ptr;
extern uint8_t vlan_names[];
extern int n_igmp_on, n_igmp_off, n_hostdef, n_dhcp_start, n_syslog_start, n_syslog_stop;

void fake_flash_reset(void);
void stp_test_defaults(void);

static void reset_all(void)
{
	hw_reset();
	for (int lp = 0; lp < 9; lp++)	/* MTU registers power up at 16383 */
		hw_reg_set(RTL8373_REG_MAC_L2_PORT_MAX_LEN + (lp << 8), 0x3fff);
	fake_flash_reset();
	env_cli_reset();
	stp_test_defaults();
	vlan_setup();
	sw_init();
	cli_init();
	management_vlan = 1;
	strcpy(hostname, "sw");
}

/* Decoders for the VLAN word vlan_create() writes (see test_port_tables):
 * bit 25 valid, bits 0-9 members, bits 10-19 untag set = ~tagged. */
static int vl_valid(uint16_t vid) { return (hw_vlan_word(vid) >> 25) & 1; }
static int vl_member(uint16_t vid, int lp) { return (hw_vlan_word(vid) >> lp) & 1; }
static int vl_tagged(uint16_t vid, int lp)
{
	return vl_member(vid, lp) && !((hw_vlan_word(vid) >> (10 + lp)) & 1);
}

/* Enter config-if for user port N from any mode */
static void to_if(const char *ifname)
{
	char b[64];
	run("end");
	run("configure terminal");
	snprintf(b, sizeof(b), "interface %s", ifname);
	run(b);
}

static int out_has(const char *needle)
{
	return strstr(out_buf, needle) != NULL;
}

extern uint8_t fake_cfg[];
extern struct telnet_state_t telnet_state;
extern char passwd[21];
extern struct dhcp_state dhcp_state;

/* The sector holds a NUL-terminated config that starts like one */
static int saved_ok(void)
{
	return out_has("[OK]") && strncmp((char *)fake_cfg, "!\nhostname ", 11) == 0
	       && memchr(fake_cfg, 0, CONFIG_LEN) != NULL;
}

/* Everything back to the power-on state, including the settings the
 * swcfg reset does not own */
static void wipe_all(void)
{
	reset_all();
	telnet_state.enabled = 0;
	telnet_state.idle_secs = TELNET_IDLE_DEFAULT;
	strcpy(passwd, "1234");
	memset(port_names, 0, 9 * PORT_NAME_SIZE);
	memset(uip_hostaddr, 0, sizeof(uip_hostaddr));
	memset(uip_netmask, 0, sizeof(uip_netmask));
	memset(uip_draddr, 0, sizeof(uip_draddr));
	syslog_state.enabled = 0;
	syslog_state.server_port = 514;
	dhcp_state.state = 0;
}

static char render_a[CONFIG_LEN], render_b[CONFIG_LEN];

static int port_of_lag_none(int lp) { return port_lag_of(lp) == PORT_LAG_NONE; }

static void render_into(char *dst)
{
	uint16_t n = runcfg_render();
	memcpy(dst, cfg_buf, n == 0xffff ? 0 : n + 1);
	if (n == 0xffff)
		dst[0] = 0;
}

/* Feed a NUL-terminated config text through the boot replay, line by
 * line, like execute_config() does */
static void replay_text(const char *t)
{
	char line[CMD_BUF_SIZE];
	int n = 0;

	cli_replay_begin();
	for (;; t++) {
		if (*t == '\n' || *t == 0) {
			line[n] = 0;
			if (n)
				cli_replay_line(line);
			n = 0;
			if (!*t)
				break;
			continue;
		}
		if (n < CMD_BUF_SIZE - 1)
			line[n++] = *t;
	}
	cli_replay_end();
}

static void test_modes(void)
{
	printf("[test] mode transitions\n");
	reset_all();
	CHECK(cli.mode == CLI_MODE_EXEC, "boots in user EXEC");
	run("enable");
	CHECK(cli.mode == CLI_MODE_PRIV, "enable enters privileged EXEC");
	run("configure terminal");
	CHECK(cli.mode == CLI_MODE_CONFIG, "configure terminal enters config");
	run("interface ethernet 1/5");
	CHECK(cli.mode == CLI_MODE_IF, "interface enters config-if");
	CHECK(cli.ctx_if == 5, "interface context is port 5");
	run("exit");
	CHECK(cli.mode == CLI_MODE_CONFIG, "exit leaves the submode");
	run("vlan 100");
	CHECK(cli.mode == CLI_MODE_VLAN, "vlan enters config-vlan");
	CHECK(cli.ctx_vlan == 100, "vlan context is 100");
	run("end");
	CHECK(cli.mode == CLI_MODE_PRIV, "end returns to privileged EXEC");
	run("disable");
	CHECK(cli.mode == CLI_MODE_EXEC, "disable returns to user EXEC");
}

static void test_abbreviation(void)
{
	printf("[test] abbreviation\n");
	reset_all();
	run("en");
	CHECK(cli.mode == CLI_MODE_PRIV, "'en' is unique for enable");
	run("conf t");
	CHECK(cli.mode == CLI_MODE_CONFIG, "'conf t' works");
	run("int e1/3");
	CHECK(cli.mode == CLI_MODE_IF && cli.ctx_if == 3,
	      "'int e1/3' selects port 3 via the single-token form");
	run("end");
	run("sh ver");
	CHECK(out_has("rtl-swos") && out_has("Uptime:"), "'sh ver' runs show version");
}

static void test_errors(void)
{
	printf("[test] error handling\n");
	reset_all();
	run("e");
	CHECK(out_has("% Ambiguous command"), "'e' is ambiguous (enable/exit)");
	run("enable");
	run("configure");
	CHECK(out_has("% Incomplete command"), "'configure' alone is incomplete");
	run("show bogus");
	CHECK(out_has("'^' marker"), "unknown subword prints the marker error");
	CHECK(out_buf[0] != '^' && strchr(out_buf, '^') != NULL,
	      "marker line is indented");
}

static void test_priv_gating(void)
{
	printf("[test] privilege gating\n");
	reset_all();
	run("reload");
	CHECK(n_reset == 0 && out_has("'^' marker"), "reload is hidden in user EXEC");
	run("enable");
	run("reload");
	CHECK(n_reset == 1, "reload works in privileged EXEC");
}

static void test_old_syntax(void)
{
	printf("[test] old flat syntax is rejected\n");
	reset_all();
	run("stat");
	CHECK(out_has("^\n% Invalid input"), "an unknown first word is invalid input");
	run("enable");
	run("configure terminal");
	run("port 5 1g");
	CHECK(out_has("'^' marker"), "old flat syntax is invalid input");
}

static void test_exec_anywhere(void)
{
	printf("[test] exec commands from config modes (no 'do')\n");
	reset_all();
	run("enable");
	run("configure terminal");
	run("show version");
	CHECK(out_has("Hardware:"), "show version works in config mode");
	CHECK(cli.mode == CLI_MODE_CONFIG, "mode unchanged");
	fake_flash_reset();
	run("write memory");
	CHECK(saved_ok(), "write memory works in config mode");
}

static void test_write_and_copy(void)
{
	printf("[test] write memory / copy running-config startup-config\n");
	reset_all();
	run("enable");
	fake_flash_reset();
	run("write");
	CHECK(saved_ok(), "bare 'write' saves");
	fake_flash_reset();
	run("copy running-config startup-config");
	CHECK(saved_ok(), "copy run start saves");
	run("copy tftp flash 10.0.0.1 fw.bin");
	CHECK(last_tftp_op == TFTP_OP_GET_FW && last_tftp_srv[0] == 10 && last_tftp_srv[3] == 1
	      && strcmp(last_tftp_file, "fw.bin") == 0, "copy tftp flash, native");
	run("copy tftp startup-config 10.0.0.2 lab.cfg");
	CHECK(last_tftp_op == TFTP_OP_GET_CONFIG && strcmp(last_tftp_file, "lab.cfg") == 0,
	      "copy tftp startup-config");
	run("copy tftp config 10.0.0.2 x.cfg");
	CHECK(last_tftp_op == TFTP_OP_GET_CONFIG && strcmp(last_tftp_file, "x.cfg") == 0,
	      "config is an alias of startup-config");
	run("copy startup-config tftp 10.0.0.3 out.cfg");
	CHECK(last_tftp_op == TFTP_OP_PUT_CONFIG && last_tftp_srv[3] == 3, "copy startup-config tftp");
	run("copy tftp flash 10.0.0.1");
	CHECK(out_has("% Incomplete command"), "a file name is required");
}

static void test_help(void)
{
	printf("[test] context help\n");
	reset_all();
	out_reset();
	strcpy(linebuf, "");
	cli_help(linebuf);
	CHECK(out_has("enable"), "root help lists enable");
	CHECK(!out_has("reload"), "root help hides privileged commands");
	run("enable");
	out_reset();
	strcpy(linebuf, "show ");
	cli_help(linebuf);
	CHECK(out_has("version"), "'show ?' lists version");
	out_reset();
	strcpy(linebuf, "sh");
	cli_help(linebuf);
	CHECK(out_has("show"), "'sh?' completes to show");
	run("configure terminal");
	out_reset();
	strcpy(linebuf, "");
	cli_help(linebuf);
	CHECK(out_has("interface"), "config help lists interface");
	CHECK(out_has("show"), "config help lists exec commands too");
}

static void test_complete(void)
{
	printf("[test] tab completion\n");
	reset_all();
	strcpy(linebuf, "ena");
	CHECK(cli_complete(linebuf, CMD_BUF_SIZE) == 4, "'ena' -> 'enable '");
	CHECK(strcmp(linebuf, "enable ") == 0, "completion text is right");
	strcpy(linebuf, "e");
	CHECK(cli_complete(linebuf, CMD_BUF_SIZE) == 0, "ambiguous 'e' does not complete");
	run("enable");
	strcpy(linebuf, "copy run");
	CHECK(cli_complete(linebuf, CMD_BUF_SIZE) > 0, "'copy run' completes");
	CHECK(strcmp(linebuf, "copy running-config ") == 0, "to running-config");
}

static void test_no_prefix(void)
{
	printf("[test] no prefix\n");
	reset_all();
	run("enable");
	run("configure terminal");
	run("no interface ethernet 1/2");
	CHECK(out_has("'^' marker"), "no on a command without NO_OK errors");
	CHECK(cli.mode == CLI_MODE_CONFIG, "mode unchanged");
	run("no");
	CHECK(out_has("% Incomplete command"), "bare 'no' is incomplete");
}

static void test_interface_config(void)
{
	printf("[test] interface-mode config commands\n");
	reset_all();
	run("enable");
	run("configure terminal");
	run("interface ethernet 1/2");
	CHECK(cli.mode == CLI_MODE_IF && cli.ctx_if == 2, "in config-if for port 2");

	run("shutdown");
	CHECK(n_setspeed == 1 && last_port == 1 && last_speed == PHY_OFF,
	      "shutdown -> phy off on logical port 1 (0-based)");
	run("no shutdown");
	CHECK(last_speed == PHY_SPEED_AUTO, "no shutdown -> auto");

	run("speed 1000");
	CHECK(last_speed == PHY_SPEED_1G && last_port == 1, "speed 1000 -> 1G");
	run("speed 2500");
	CHECK(last_speed == PHY_SPEED_2G5, "speed 2500 -> 2.5G");
	run("speed auto");
	CHECK(last_speed == PHY_SPEED_AUTO, "speed auto");
	run("sp 100");
	CHECK(out_has("% Ambiguous command"), "'sp' is ambiguous (speed, spanning-tree)");
	run("spe 100");
	CHECK(last_speed == PHY_SPEED_100M, "abbreviated 'spe 100' -> 100M");

	run("description lab uplink port");
	CHECK(strcmp(port_names[1], "lab uplink port") == 0,
	      "description stores the rest of the line with spaces");
	run("no description");
	CHECK(port_names[1][0] == 0, "no description clears the name");
}

static void test_vlan_db(void)
{
	printf("[test] vlan database + config-vlan\n");
	reset_all();
	run("enable");
	run("configure terminal");
	run("vlan 10");
	CHECK(cli.mode == CLI_MODE_VLAN && cli.ctx_vlan == 10, "vlan 10 enters config-vlan");
	CHECK(sw_vlan_exists(10), "vlan 10 is in the database");
	CHECK(vl_valid(10), "and has a valid hardware entry");
	CHECK(vl_member(10, 9), "the CPU port is a member");
	run("name home");
	CHECK(vlan_name(10) != 0xffff && strncmp((char *)&vlan_names[vlan_name(10)], "home ", 5) == 0,
	      "name stored in the shared vlan name table");
	run("name bad!name");
	CHECK(out_has("% Invalid name"), "names are validated");
	run("no name");
	CHECK(vlan_name(10) == 0xffff, "no name removes it");
	run("exit");
	run("no vlan 10");
	CHECK(!sw_vlan_exists(10) && !vl_valid(10), "no vlan removes db entry and hardware entry");
	run("no vlan 1");
	CHECK(out_has("% Default VLAN 1 may not be deleted"), "vlan 1 is protected");
	CHECK(sw_vlan_exists(1), "vlan 1 still there");
}

static void test_switchport_access(void)
{
	printf("[test] switchport access\n");
	reset_all();
	run("enable");
	to_if("ethernet 1/3");
	CHECK(cli.ctx_lport == 2, "user port 3 is logical port 2 on the identity board");
	run("switchport access vlan 20");
	CHECK(out_has("% VLAN 20 did not exist, created it"), "access vlan auto-creates");
	CHECK(vl_member(20, 2) && !vl_tagged(20, 2), "port untagged member of vlan 20");
	CHECK(!vl_member(1, 2), "and removed from vlan 1");
	CHECK(vl_member(1, 0) && vl_member(1, 3), "other ports stay in vlan 1");
	CHECK(port_pvid_get(2) == 20, "PVID follows the access vlan");
	CHECK(port_ingress_filter_get(2) == VLAN_UNTAGGED, "access port accepts untagged only");
	run("switchport access vlan");
	CHECK(out_has("% Incomplete command"), "bare 'switchport access vlan' is incomplete");
	run("no switchport access vlan");
	CHECK(vl_member(1, 2) && !vl_member(20, 2) && port_pvid_get(2) == 1,
	      "no switchport access vlan returns the port to vlan 1");
}

static void test_switchport_trunk(void)
{
	printf("[test] switchport trunk\n");
	reset_all();
	run("enable");
	run("configure terminal");
	run("vlan 20");
	run("vlan 30");
	to_if("ethernet 1/9");			/* logical 8 */
	run("switchport mode trunk");
	CHECK(vl_member(1, 8) && !vl_tagged(1, 8), "trunk: native vlan 1 untagged");
	CHECK(vl_tagged(20, 8) && vl_tagged(30, 8), "trunk: other vlans tagged (allowed all)");
	CHECK(port_ingress_filter_get(8) == VLAN_ALL, "trunk accepts tagged and untagged");

	run("switchport trunk allowed vlan 20");
	CHECK(vl_tagged(20, 8) && !vl_member(30, 8) && !vl_member(1, 8),
	      "allowed vlan LIST replaces the list");
	run("switchport trunk allowed vlan add 1,30");
	CHECK(vl_member(1, 8) && vl_tagged(30, 8) && vl_tagged(20, 8), "allowed vlan add");
	run("switchport trunk allowed vlan remove 20");
	CHECK(!vl_member(20, 8) && vl_member(30, 8), "allowed vlan remove");

	run("switchport trunk native vlan 30");
	CHECK(vl_member(30, 8) && !vl_tagged(30, 8), "native vlan is untagged");
	CHECK(vl_tagged(1, 8), "old native now tagged");
	CHECK(port_pvid_get(8) == 30, "PVID follows the native vlan");

	run("switchport trunk allowed vlan 10-");
	CHECK(out_has("% Invalid VLAN list"), "malformed list rejected");
	CHECK(vl_member(30, 8) && vl_member(1, 8), "and the list is unchanged");
	run("switchport trunk allowed vlan 1,3,5,7,9,11,13,15,17");
	CHECK(out_has("% Too many VLAN ranges"), "more than 8 ranges rejected");
	CHECK(vl_member(30, 8), "list unchanged after overflow");
	run("switchport trunk allowed vlan 5000");
	CHECK(out_has("out of range"), "vid above 4094 rejected");

	run("switchport trunk allowed vlan none");
	CHECK(!vl_member(1, 8) && !vl_member(20, 8) && !vl_member(30, 8), "allowed vlan none");
	run("no switchport trunk allowed vlan");
	CHECK(vl_member(20, 8) && vl_member(30, 8), "no allowed vlan -> all");
	run("no switchport mode");
	CHECK(vl_member(1, 8) && !vl_member(20, 8) && port_ingress_filter_get(8) == VLAN_UNTAGGED,
	      "no switchport mode -> access vlan 1");
}

static void test_range_math(void)
{
	printf("[test] allowed-list range compression\n");
	reset_all();
	CHECK(sw_allowed_edit(4, SW_AL_SET, "10-20,21,22-25,40") == SW_OK, "set list");
	CHECK(sw_ports[4].nranges == 2 && sw_ports[4].allowed[0].lo == 10 &&
	      sw_ports[4].allowed[0].hi == 25 && sw_ports[4].allowed[1].lo == 40,
	      "adjacent ranges merge: 10-25,40");
	CHECK(sw_allowed_edit(4, SW_AL_REMOVE, "15") == SW_OK && sw_ports[4].nranges == 3,
	      "removing an inner vid splits the range");
	CHECK(sw_port_allows(4, 14) && !sw_port_allows(4, 15) && sw_port_allows(4, 16),
	      "membership after the split");
	CHECK(sw_allowed_edit(4, SW_AL_ADD, "4094") == SW_OK && sw_port_allows(4, 4094),
	      "top vid 4094");
	CHECK(sw_allowed_edit(4, SW_AL_SET, "0") == SW_ERR_RANGE, "vid 0 rejected");
	CHECK(sw_allowed_edit(4, SW_AL_SET, "30-20") == SW_ERR_RANGE, "reversed range rejected");
	CHECK(sw_allowed_edit(4, SW_AL_SET, "1,,2") == SW_ERR_SYNTAX, "empty element rejected");
	CHECK(sw_port_allows(4, 4094), "failed edits leave the list alone");
}

static void test_global_config(void)
{
	printf("[test] global config: hostname, svi, gateway, igmp, logging, mtu\n");
	reset_all();
	run("enable");
	run("configure terminal");

	run("hostname core-sw1");
	CHECK(strcmp(hostname, "core-sw1") == 0, "hostname set");
	run("hostname bad!name");
	CHECK(out_has("% Invalid hostname") && strcmp(hostname, "core-sw1") == 0,
	      "invalid hostname rejected");
	run("hostname");
	CHECK(out_has("% Incomplete command"), "bare hostname is incomplete");
	run("no hostname");
	CHECK(n_hostdef == 1 && strcmp(hostname, "default") == 0, "no hostname restores the default");

	run("interface vlan 10");
	CHECK(cli.mode == CLI_MODE_SVI && cli.ctx_vlan == 10, "interface vlan enters SVI mode");
	run("ip address 192.168.10.5 255.255.255.0");
	CHECK(sw_vlan_exists(10) && vl_valid(10), "svi vlan auto-created");
	CHECK(management_vlan == 10, "management vlan follows the svi");
	{
		uip_ipaddr_t a, m;
		uip_ipaddr(&a, 192, 168, 10, 5);
		uip_ipaddr(&m, 255, 255, 255, 0);
		CHECK(uip_ipaddr_cmp(uip_hostaddr, a) && uip_ipaddr_cmp(uip_netmask, m),
		      "address and mask applied");
	}
	run("ip address 192.168.10.5");
	CHECK(out_has("% Incomplete command"), "address without mask is incomplete");
	run("ip address dhcp");
	CHECK(n_dhcp_start == 1, "ip address dhcp starts the client");
	run("exit");

	run("ip default-gateway 192.168.10.1");
	{
		uip_ipaddr_t g;
		uip_ipaddr(&g, 192, 168, 10, 1);
		CHECK(uip_ipaddr_cmp(uip_draddr, g), "default gateway applied");
	}
	run("ip igmp snooping");
	CHECK(n_igmp_on == 1, "ip igmp snooping");
	run("no ip igmp snooping");
	CHECK(n_igmp_off == 1, "no ip igmp snooping");

	run("logging host 10.0.0.9");
	CHECK(n_syslog_start == 1 && syslog_state.server_port == 514 &&
	      syslog_state.server_ip[0] == 10 && syslog_state.server_ip[3] == 9,
	      "logging host with default port");
	run("logging host 10.0.0.9 port 1514");
	CHECK(n_syslog_stop == 1 && n_syslog_start == 2 && syslog_state.server_port == 1514,
	      "logging host with port restarts the client");
	run("no logging host");
	CHECK(n_syslog_stop == 2 && !syslog_state.enabled, "no logging host");

	to_if("ethernet 1/4");
	run("mtu 9000");
	CHECK((hw_reg_get(RTL8373_REG_MAC_L2_PORT_MAX_LEN + (3 << 8)) & 0x3fff) == 9000,
	      "mtu written to logical port 3");
	run("mtu 20");
	CHECK(out_has("'^' marker"), "mtu below 64 rejected");
}

static void test_no_edge_cases(void)
{
	printf("[test] no-prefix edge cases + help\n");
	reset_all();
	run("enable");
	run("configure terminal");
	run("n vlan 5");
	CHECK(out_has("'^' marker") && !sw_vlan_exists(5), "a lone 'n' is not taken for 'no'");
	to_if("ethernet 1/2");
	out_reset();
	strcpy(linebuf, "switchport access vlan ");
	cli_help(linebuf);
	CHECK(out_has("<1-4094>") && !out_has("<cr>"), "plain form offers the vid, no <cr>");
	out_reset();
	strcpy(linebuf, "no switchport access vlan ");
	cli_help(linebuf);
	CHECK(out_has("<cr>"), "no form offers <cr>");
	out_reset();
	strcpy(linebuf, "switchport ");
	cli_help(linebuf);
	CHECK(out_has("access") && out_has("mode") && out_has("trunk"), "switchport ? lists subcommands");
	strcpy(linebuf, "no sw");
	CHECK(cli_complete(linebuf, CMD_BUF_SIZE) > 0 && strcmp(linebuf, "no switchport ") == 0,
	      "tab completion works after no");
}

static void test_block_replay(void)
{
	printf("[test] block config replays without exit lines\n");
	reset_all();
	run("enable");
	run("configure terminal");
	static const char *cfg[] = {
		"hostname lab-sw",
		"vlan 10",
		" name home",
		"vlan 20",			/* global command from config-vlan */
		" name work",
		"interface ethernet 1/2",	/* from config-vlan */
		" description uplink",
		" switchport mode trunk",
		" switchport trunk allowed vlan 10,20",
		"interface ethernet 1/3",	/* from config-if */
		" switchport access vlan 10",
		"interface vlan 10",
		" ip address 192.168.10.247 255.255.255.0",
		"ip default-gateway 192.168.10.1",	/* from SVI mode */
		0
	};
	for (int i = 0; cfg[i]; i++) {
		run(cfg[i]);
		if (out_has("%")) {
			printf("    line '%s' -> %s", cfg[i], out_buf);
		}
	}
	CHECK(strcmp(hostname, "lab-sw") == 0, "hostname");
	CHECK(vlan_name(20) != 0xffff && strncmp((char *)&vlan_names[vlan_name(20)], "work ", 5) == 0,
	      "vlan 20 named after hopping from config-vlan 10");
	CHECK(strcmp(port_names[1], "uplink") == 0, "port 2 description");
	CHECK(vl_tagged(10, 1) && vl_tagged(20, 1) && !vl_member(1, 1), "port 2 trunk 10,20");
	CHECK(vl_member(10, 2) && !vl_tagged(10, 2), "port 3 access 10 (entered from config-if)");
	CHECK(management_vlan == 10, "svi on vlan 10");
	{
		uip_ipaddr_t g;
		uip_ipaddr(&g, 192, 168, 10, 1);
		CHECK(uip_ipaddr_cmp(uip_draddr, g), "gateway parsed from SVI mode");
	}
	CHECK(cli.mode == CLI_MODE_CONFIG, "global command left the submode");
	run("vlan");
	CHECK(out_has("% Incomplete command"), "incomplete global command from a submode reports incomplete");
	to_if("ethernet 1/2");
	run("shutdown bogus");
	CHECK(out_has("'^' marker") && cli.mode == CLI_MODE_IF,
	      "invalid in the submode and unknown globally: error, stays in the submode");
	strcpy(linebuf, "ex");
	CHECK(cli_complete(linebuf, CMD_BUF_SIZE) > 0 && strcmp(linebuf, "exit ") == 0,
	      "'ex' completes although exit is in several roots");
}

static void test_runcfg_defaults(void)
{
	printf("[test] running-config of a factory-default switch\n");
	wipe_all();
	uip_ipaddr(&uip_hostaddr, 192, 168, 10, 247);
	uip_ipaddr(&uip_netmask, 255, 255, 255, 0);
	render_into(render_a);
	CHECK(strncmp(render_a, "!\nhostname sw\n!\ninterface ethernet 1/1\n!\n", 41) == 0,
	      "hostname then bare interfaces");
	CHECK(!strstr(render_a, "\nvlan 1\n"), "unnamed default vlan not listed");
	CHECK(strstr(render_a, "interface vlan 1\n ip address 192.168.10.247 255.255.255.0\n"),
	      "management interface always listed");
	CHECK(!strstr(render_a, "feature telnet") && !strstr(render_a, "line vty"),
	      "defaults for telnet and the vty are not listed");
	CHECK(!strstr(render_a, " mtu ") && !strstr(render_a, " speed "), "no per-port defaults");
	CHECK(strstr(render_a, "interface ethernet 1/9\n!\n") != NULL, "all 9 ports listed");
	CHECK(strcmp(render_a + strlen(render_a) - 5, "!\nend") == 0 ||
	      strcmp(render_a + strlen(render_a) - 6, "!\nend\n") == 0, "ends with end");
}

static void test_runcfg_roundtrip(void)
{
	printf("[test] running-config round trip through the boot replay\n");
	wipe_all();
	run("enable");
	run("configure terminal");
	static const char *cfg[] = {
		"hostname lab-sw",
		"vlan 10", " name home", "vlan 20", " name work", "vlan 40",
		"interface ethernet 1/1", " description to-desk", " switchport access vlan 20",
		"interface ethernet 1/2", " switchport mode trunk",
		" switchport trunk allowed vlan 10,20,40",
		"interface ethernet 1/3", " speed 1000", " shutdown", " mtu 9000",
		"interface ethernet 1/9", " switchport mode trunk",
		" switchport trunk native vlan 40", " switchport trunk allowed vlan 1-20,40",
		"interface vlan 10", " ip address 192.168.10.247 255.255.254.0",
		"ip default-gateway 192.168.10.1",
		"ip igmp snooping",
		"logging host 10.0.0.9 port 1514",
		"feature telnet",
		"line vty 0 4", " exec-timeout 30 0", " password s3cret",
		0
	};
	for (int i = 0; cfg[i]; i++) {
		run(cfg[i]);
		if (out_has("%"))
			printf("    line '%s' -> %s", cfg[i], out_buf);
	}
	render_into(render_a);
	uint32_t v1 = hw_vlan_word(1), v10 = hw_vlan_word(10), v20 = hw_vlan_word(20), v40 = hw_vlan_word(40);
	uint16_t pv0 = port_pvid_get(0), pv8 = port_pvid_get(8);

	CHECK(strstr(render_a, "vlan 10\n name home\nvlan 20\n name work\nvlan 40\n!\n") != NULL,
	      "vlans in ascending order with names");
	CHECK(strstr(render_a, "interface ethernet 1/3\n shutdown\n speed 1000\n mtu 9000\n!\n") != NULL,
	      "per-port physical settings");
	CHECK(strstr(render_a, " switchport trunk native vlan 40\n switchport trunk allowed vlan 1-20,40\n"),
	      "trunk native + allowed ranges");
	CHECK(strstr(render_a, "interface vlan 10\n ip address 192.168.10.247 255.255.254.0\n"),
	      "svi");
	CHECK(strstr(render_a, "line vty\n exec-timeout 30 0\n password s3cret\n"), "vty block");
	CHECK(strstr(render_a, "logging host 10.0.0.9 port 1514\n") && strstr(render_a, "feature telnet\n"),
	      "services");

	wipe_all();
	CHECK(hw_vlan_word(10) == 0 && !telnet_state.enabled, "state really wiped");
	replay_text(render_a);
	CHECK(cli.mode == CLI_MODE_EXEC, "replay ends in user EXEC");
	render_into(render_b);
	CHECK(strcmp(render_a, render_b) == 0, "re-rendered config is byte-identical");
	if (strcmp(render_a, render_b))
		printf("--- before ---\n%s--- after ---\n%s", render_a, render_b);
	CHECK(hw_vlan_word(1) == v1 && hw_vlan_word(10) == v10 && hw_vlan_word(20) == v20
	      && hw_vlan_word(40) == v40, "VLAN table words identical after replay");
	CHECK(port_pvid_get(0) == pv0 && port_pvid_get(8) == pv8, "PVIDs identical after replay");
	CHECK(telnet_state.enabled && telnet_state.idle_secs == 1800 && strcmp(passwd, "s3cret") == 0,
	      "telnet, timeout and password restored");
}

static void test_write_and_startup(void)
{
	printf("[test] write memory -> sector -> show startup-config\n");
	wipe_all();
	run("enable");
	run("configure terminal");
	run("hostname saved-sw");
	run("end");
	run("write memory");
	CHECK(out_has("Building configuration") && out_has("[OK]"), "write memory reports");
	render_into(render_a);
	CHECK(strcmp((char *)fake_cfg, render_a) == 0, "sector holds the rendered config");
	run("show startup-config");
	CHECK(strcmp(out_buf, render_a) == 0, "show startup-config prints it back");
	run("show running-config");
	CHECK(strcmp(out_buf, render_a) == 0, "show running-config renders the same text");
}

static void test_replay_legacy_and_comments(void)
{
	printf("[test] boot replay: bad lines reported and skipped, comments, deferred push\n");
	wipe_all();
	unsigned long w0;
	out_reset();
	replay_text("! a comment\n"
		    "ip 192.168.10.247\n"		/* legacy: invalid new syntax */
		    "netmask 255.255.255.0\n"		/* legacy: unknown word */
		    "telnet on\n"
		    "   ! indented comment\n"
		    "vlan 30\n"				/* new syntax still works */
		    " name lab\n");
	CHECK(out_has("% In the startup config:\nip 192.168.10.247\n") && out_has("netmask 255.255.255.0"),
	      "old-syntax lines are reported with the line itself");
	CHECK(sw_vlan_exists(30) && vl_valid(30), "new-syntax lines in the same replay work");
	CHECK(vlan_name(30) != 0xffff, "including a submode line");

	/* sw_apply is deferred during replay: a replayed trunk config pushes once */
	wipe_all();
	cli_replay_begin();
	w0 = hw_writes;
	cli_replay_line("interface ethernet 1/2");
	cli_replay_line(" switchport mode trunk");
	CHECK(hw_writes == w0, "no hardware writes while deferred");
	cli_replay_end();
	CHECK(hw_writes > w0 && vl_tagged(1, 1) == 0 && vl_member(1, 1), "pushed at the end");

	/* interactive: an invalid line is an error, not a legacy command */
	wipe_all();
	run("enable");
	run("configure terminal");
	run("ip 192.168.10.247");
	CHECK(out_has("'^' marker"), "interactive lines in the old syntax are invalid input");
}

static void test_physical_shadows(void)
{
	printf("[test] speed/shutdown/mtu shadows\n");
	wipe_all();
	run("enable");
	to_if("ethernet 1/4");
	run("speed 1000");
	CHECK(last_speed == PHY_SPEED_1G, "speed applied");
	run("shutdown");
	CHECK(last_speed == PHY_OFF, "shutdown");
	run("speed 100");
	CHECK(last_speed == PHY_OFF && sw_ports[3].speed == PHY_SPEED_100M,
	      "speed on a shut port is stored, the port stays down");
	run("no shutdown");
	CHECK(last_speed == PHY_SPEED_100M, "no shutdown restores the configured speed");
	run("no speed");
	CHECK(last_speed == PHY_SPEED_AUTO && sw_ports[3].speed == PHY_SPEED_AUTO, "no speed -> auto");
	run("mtu 9000");
	run("no mtu");
	CHECK((hw_reg_get(RTL8373_REG_MAC_L2_PORT_MAX_LEN + (3 << 8)) & 0x3fff) == 16383, "no mtu -> 16383");
	run("end");
	run("configure terminal");
	run("line vty");
	CHECK(cli.mode == CLI_MODE_LINE, "line vty mode");
	run("exec-timeout 0 0");
	CHECK(telnet_state.idle_secs == 0xffff, "0 0 = never");
	render_into(render_a);
	CHECK(strstr(render_a, " exec-timeout 0 0\n") != NULL, "renders back as 0 0");
	run("exec-timeout 0 10");
	CHECK(out_has("Minimum timeout") && telnet_state.idle_secs == 0xffff, "below 30s rejected");
	run("no exec-timeout");
	CHECK(telnet_state.idle_secs == TELNET_IDLE_DEFAULT, "no exec-timeout -> default");
	run("password abc");
	run("no password");
	CHECK(strcmp(passwd, "1234") == 0, "no password -> default");
}

extern uint32_t bw_in[10], bw_out[10];
extern uint8_t bw_in_drop[10];
extern int n_stp_enable, n_stp_disable, n_stp_prio;
extern bool stp_enabled;
extern uint16_t igmp_mrouter;
extern int n_stp_sync;

static void test_port_features(void)
{
	printf("[test] eee, protected, rate-limit\n");
	wipe_all();
	run("enable");
	to_if("ethernet 1/2");
	run("no power efficient-ethernet");
	CHECK(sw_ports[1].eee_off, "no power efficient-ethernet");
	run("power efficient-ethernet");
	CHECK(out_has("% Incomplete command"), "plain form needs auto");
	run("power efficient-ethernet auto");
	CHECK(!sw_ports[1].eee_off, "power efficient-ethernet auto");

	run("switchport protected");
	to_if("ethernet 1/3");
	run("switchport protected");
	CHECK(!(port_isolation_get(1) & (1 << 2)) && !(port_isolation_get(2) & (1 << 1)),
	      "two protected ports cannot reach each other");
	CHECK((port_isolation_get(1) & (1 << 4)) && (port_isolation_get(1) & 0x200),
	      "a protected port still reaches others and the CPU");
	CHECK((port_isolation_get(4) & 0x6) == 0x6, "an unprotected port reaches the protected ones");
	run("no switchport protected");
	CHECK(port_isolation_get(1) & (1 << 2), "no switchport protected");

	run("rate-limit input 1000");
	CHECK(bw_in[2] == 992 && !bw_in_drop[2], "input limit, rounded to 16 kbit/s, pause mode");
	run("rate-limit input 5000 drop");
	CHECK(bw_in[2] == 4992 && bw_in_drop[2], "input limit in drop mode");
	run("rate-limit output 250000");
	CHECK(bw_out[2] == 250000, "output limit");
	run("rate-limit input 8");
	CHECK(out_has("% Value out of range") && bw_in[2] == 4992, "below 16 kbit/s rejected");
	run("no rate-limit output");
	CHECK(bw_out[2] == 0 && bw_in[2] == 4992, "no rate-limit output leaves the input limit");
}

static void test_monitor(void)
{
	printf("[test] monitor session\n");
	wipe_all();
	run("enable");
	run("configure terminal");
	run("monitor session 1 source interface ethernet 1/2 rx");
	run("monitor session 1 source interface e1/3");
	run("monitor session 1 destination interface ethernet 1/9");
	CHECK(sw_mon_dst == 8 && sw_mon_rx == 0x6 && sw_mon_tx == 0x4, "sources and destination");
	run("monitor session 1 source interface ethernet 1/9");
	CHECK(out_has("destination cannot be a source"), "destination is not a source");
	run("monitor session 1 destination interface ethernet 1/3");
	CHECK(sw_mon_dst == 2 && !(sw_mon_rx & 4) && !(sw_mon_tx & 4),
	      "moving the destination onto a source removes it as a source");
	run("no monitor session 1 source interface ethernet 1/2");
	CHECK(!sw_mon_rx && !sw_mon_tx, "no ... source");
	run("monitor session 1 source interface ethernet 1/2 tx");
	run("no monitor session 1");
	CHECK(sw_mon_dst == SW_MON_NONE && !sw_mon_rx && !sw_mon_tx, "no monitor session 1");
}

static void test_port_channel(void)
{
	printf("[test] port-channels\n");
	wipe_all();
	run("enable");
	to_if("ethernet 1/7");
	run("channel-group 2 mode on");
	to_if("ethernet 1/8");
	run("channel-group 2");
	CHECK(port_lag_members_get(1) == 0xc0, "ports 7 and 8 in lag 2");
	CHECK((hw_reg_get(RTL837X_TRK_HASH_CTRL_BASE + 4) & 0xff) == LAG_HASH_DEFAULT,
	      "joining a pristine lag installs the default hash");
	run("channel-group 3 mode on");
	CHECK(port_lag_members_get(1) == 0x40 && port_lag_members_get(2) == 0x80,
	      "channel-group moves the port between lags");
	run("no channel-group");
	CHECK(port_lag_members_get(2) == 0 && port_of_lag_none(7), "no channel-group");
	run("end");
	run("configure terminal");
	run("interface port-channel 2");
	CHECK(cli.mode == CLI_MODE_PO && cli.ctx_po == 2, "port-channel mode");
	run("load-balance src-mac dst-mac src-ip");
	CHECK((hw_reg_get(RTL837X_TRK_HASH_CTRL_BASE + 4) & 0xff)
	      == (LAG_HASH_L2_SMAC | LAG_HASH_L2_DMAC | LAG_HASH_L3_SIP), "hash field list");
	run("load-balance");
	CHECK(out_has("% Incomplete command"), "load-balance needs fields");
	run("no load-balance");
	CHECK((hw_reg_get(RTL837X_TRK_HASH_CTRL_BASE + 4) & 0xff) == LAG_HASH_DEFAULT,
	      "no load-balance restores the default");
}

static void test_stp(void)
{
	printf("[test] spanning-tree\n");
	wipe_all();
	run("enable");
	run("configure terminal");
	run("spanning-tree priority 4096");
	CHECK(stp_prio == 0x10 && n_stp_prio == 1, "priority");
	run("spanning-tree priority 5000");
	CHECK(out_has("% Value out of range") && stp_prio == 0x10, "priority must step by 4096");
	run("spanning-tree mode stp");
	CHECK(stp_rstp == 0, "mode stp");
	run("no spanning-tree mode");
	CHECK(stp_rstp == 1, "no mode -> rstp");
	run("spanning-tree hello-time 11");
	CHECK(out_has("% Value out of range") && stp_hello_s == 2, "hello range");
	run("spanning-tree forward-time 10");
	run("spanning-tree max-age 30");
	run("spanning-tree transmit hold-count 3");
	CHECK(stp_fwddelay_s == 10 && stp_maxage_s == 30 && stp_txhold == 3, "timers");
	run("feature spanning-tree");
	CHECK(stp_enabled && n_stp_enable == 1, "feature spanning-tree");

	to_if("ethernet 1/1");
	run("spanning-tree portfast");
	CHECK((stp_pflags[0] & STP_PF_ADMEDGE) && !(stp_pflags[0] & STP_PF_AUTOEDGE), "portfast");
	run("spanning-tree bpduguard enable");
	run("spanning-tree guard root");
	run("spanning-tree cost 2000");
	run("spanning-tree port-priority 64");
	run("spanning-tree link-type point-to-point");
	CHECK((stp_pflags[0] & (STP_PF_BPDUGUARD | STP_PF_ROOTGUARD)) == (STP_PF_BPDUGUARD | STP_PF_ROOTGUARD)
	      && stp_pcost[0] == 2000 && stp_pprio[0] == 64 && stp_pp2p[0] == 1, "port settings");
	run("spanning-tree port-priority 65");
	CHECK(out_has("% Value out of range") && stp_pprio[0] == 64, "port-priority steps by 16");
	run("no spanning-tree portfast");
	CHECK((stp_pflags[0] & STP_PF_AUTOEDGE) && !(stp_pflags[0] & STP_PF_ADMEDGE), "no portfast -> auto edge");
	run("spanning-tree portfast disable");
	CHECK(!(stp_pflags[0] & (STP_PF_AUTOEDGE | STP_PF_ADMEDGE)), "portfast disable");

	to_if("ethernet 1/7");
	run("channel-group 1 mode on");
	run("spanning-tree cost 100");
	CHECK(out_has("configure spanning-tree under interface port-channel 1") && stp_pcost[6] == 0,
	      "a lag member refuses per-port STP");
	run("interface port-channel 1");
	run("spanning-tree cost 100");
	CHECK(stp_pcost[STP_LAG_BASE] == 100, "port-channel STP goes to the lag entity");

	run("spanning-tree priority 8192");
	CHECK(cli.mode == CLI_MODE_CONFIG && stp_prio == 0x20,
	      "a global spanning-tree line from interface mode runs globally");
	run("no feature spanning-tree");
	CHECK(!stp_enabled && n_stp_disable == 1, "no feature spanning-tree");
}

static void test_roundtrip_all(void)
{
	printf("[test] round trip with every feature\n");
	wipe_all();
	run("enable");
	run("configure terminal");
	static const char *cfg[] = {
		"vlan 10",
		"interface port-channel 2", " load-balance src-mac dst-mac",
		" spanning-tree cost 400",
		"interface ethernet 1/1", " spanning-tree portfast", " spanning-tree bpduguard enable",
		" switchport protected", " no power efficient-ethernet",
		"interface ethernet 1/2", " rate-limit input 1024 drop", " rate-limit output 20000",
		" spanning-tree link-type shared", " spanning-tree port-priority 32",
		"interface ethernet 1/3", " switchport protected", " spanning-tree portfast disable",
		"interface ethernet 1/7", " channel-group 2 mode on", " switchport mode trunk",
		"interface ethernet 1/8", " channel-group 2 mode on", " switchport mode trunk",
		"monitor session 1 source interface ethernet 1/1 tx",
		"monitor session 1 source interface ethernet 1/2",
		"monitor session 1 destination interface ethernet 1/9",
		"spanning-tree mode stp", "spanning-tree priority 4096", "spanning-tree max-age 30",
		"feature spanning-tree",
		0
	};
	for (int i = 0; cfg[i]; i++) {
		run(cfg[i]);
		if (out_has("%"))
			printf("    line '%s' -> %s", cfg[i], out_buf);
	}
	render_into(render_a);
	CHECK(strstr(render_a, "interface port-channel 2\n load-balance src-mac dst-mac\n"
			       " spanning-tree cost 400\n!\ninterface ethernet 1/1\n") != NULL,
	      "port-channel block precedes the ethernet blocks");
	CHECK(strstr(render_a, " channel-group 2 mode on\n") != NULL, "members listed");
	CHECK(strstr(render_a, "monitor session 1 source interface ethernet 1/1 tx\n"
			       "monitor session 1 source interface ethernet 1/2\n"
			       "monitor session 1 destination interface ethernet 1/9\n"), "monitor lines");
	CHECK(strstr(render_a, "spanning-tree mode stp\nspanning-tree priority 4096\n"
			       "spanning-tree max-age 30\nfeature spanning-tree\n"), "stp globals, feature last");

	uint16_t iso0 = port_isolation_get(0), lagm = port_lag_members_get(1);
	uint32_t hash = hw_reg_get(RTL837X_TRK_HASH_CTRL_BASE + 4);
	wipe_all();
	replay_text(render_a);
	render_into(render_b);
	CHECK(strcmp(render_a, render_b) == 0, "re-rendered config is byte-identical");
	if (strcmp(render_a, render_b))
		printf("--- before ---\n%s--- after ---\n%s", render_a, render_b);
	CHECK(port_isolation_get(0) == iso0 && port_lag_members_get(1) == lagm
	      && hw_reg_get(RTL837X_TRK_HASH_CTRL_BASE + 4) == hash, "isolation, lag, hash restored");
	CHECK(bw_in[1] == 1024 && bw_in_drop[1] && bw_out[1] == 20000, "rate limits restored");
	CHECK(sw_mon_dst == 8 && sw_mon_tx == 0x3 && sw_mon_rx == 0x2, "monitor restored");
	CHECK(stp_enabled && !stp_rstp && stp_prio == 0x10 && stp_pcost[STP_LAG_BASE + 1] == 400,
	      "stp restored");
}

static void test_sessions(void)
{
	printf("[test] console and vty are separate sessions\n");
	wipe_all();
	cli_use(CLI_CONSOLE);
	run("enable");
	run("configure terminal");
	run("interface ethernet 1/5");
	CHECK(cli.mode == CLI_MODE_IF && cli.ctx_if == 5, "console in config-if 1/5");
	cli_use(CLI_VTY);
	CHECK(cli.mode == CLI_MODE_EXEC, "vty starts in user EXEC");
	run("reload");
	CHECK(n_reset == 0, "and is not privileged by the console's enable");
	run("enable");
	run("configure terminal");
	run("vlan 77");
	CHECK(cli.mode == CLI_MODE_VLAN && cli.ctx_vlan == 77, "vty in config-vlan 77");
	cli_use(CLI_CONSOLE);
	CHECK(cli.mode == CLI_MODE_IF && cli.ctx_if == 5 && cli.ctx_lport == 4,
	      "console context survived the vty's commands");
	run("description still-here");
	CHECK(strcmp(port_names[4], "still-here") == 0, "and applies to its own interface");
	cli_use(CLI_VTY);
	CHECK(cli.mode == CLI_MODE_VLAN && cli.ctx_vlan == 77, "vty context survived too");
	cli_use(CLI_CONSOLE);
}

extern int n_stp_status;

static void test_show(void)
{
	printf("[test] show commands\n");
	wipe_all();
	run("enable");
	run("configure terminal");
	run("vlan 10"); run("name home");
	run("vlan 20");
	run("interface ethernet 1/1"); run("description uplink"); run("switchport access vlan 10");
	run("interface ethernet 1/2"); run("switchport access vlan 10");
	run("interface ethernet 1/3"); run("shutdown"); run("speed 1000");
	run("interface ethernet 1/6"); run("switchport mode trunk"); run("switchport trunk allowed vlan 10,20-30");
	run("interface ethernet 1/7"); run("channel-group 1 mode on");
	run("interface ethernet 1/8"); run("channel-group 1 mode on");
	run("monitor session 1 source interface ethernet 1/1 rx");
	run("monitor session 1 destination interface ethernet 1/9");
	run("end");

	run("show vlan brief");
	CHECK(strstr(out_buf, "1     default") && strstr(out_buf, "10    home") && strstr(out_buf, "20    VLAN0020"),
	      "show vlan brief: default, named and generated names");
	CHECK(strstr(out_buf, "Eth1/1, Eth1/2") != NULL, "access ports listed under their vlan");
	CHECK(!strstr(out_buf, "Eth1/6"), "trunk ports are not listed there");
	run("show vlan");
	CHECK(strstr(out_buf, "10    home") != NULL, "show vlan = show vlan brief");

	run("show interfaces status");
	CHECK(strstr(out_buf, "Eth1/1    uplink") && strstr(out_buf, "notconnect"), "status: name + link");
	CHECK(strstr(out_buf, "Eth1/3") && strstr(out_buf, "disabled") && strstr(out_buf, "1000"),
	      "status: shut port and its configured speed");
	CHECK(strstr(out_buf, "trunk") && strstr(out_buf, "Po1"), "status: trunk and lag member");
	run("show interfaces");
	CHECK(strstr(out_buf, "notconnect") != NULL, "show interfaces = status");

	run("show interfaces trunk");
	CHECK(strstr(out_buf, "Eth1/6    1       10,20-30") != NULL, "show interfaces trunk");

	hw_counter_set(1, STAT_COUNTER_RX_PKTS, 12345);
	hw_counter_set(1, STAT_COUNTER_TX_PKTS, 678);
	run("show interfaces counters");
	CHECK(strstr(out_buf, "Eth1/2    12345") && strstr(out_buf, "678"), "counters from the MIB");

	run("show port-channel summary");
	CHECK(strstr(out_buf, "Po1") && strstr(out_buf, "Eth1/7(D) Eth1/8(D)")
	      && strstr(out_buf, "src-mac dst-mac src-ip dst-ip l4-src-port l4-dst-port"),
	      "port-channel summary with members and default hash");

	run("show ip interface brief");
	CHECK(strstr(out_buf, "Vlan1") && strstr(out_buf, "static"), "ip interface brief");

	run("show monitor session 1");
	CHECK(strstr(out_buf, "Session 1 (active)") && strstr(out_buf, "Source rx:    Eth1/1")
	      && strstr(out_buf, "Destination:  Eth1/9"), "monitor session");

	run("show spanning-tree");
	CHECK(n_stp_status == 1, "show spanning-tree");
	run("show mac address-table");
	CHECK(strstr(out_buf, "MAC Address") != NULL, "mac table header");
	run("clear mac address-table dynamic");
	CHECK(!out_has("%"), "clear mac address-table dynamic");
	run("disable");
	run("clear mac address-table dynamic");
	CHECK(out_has("'^' marker"), "clear is privileged");
}


static void test_step4(void)
{
	printf("[test] debug, duplex, mac-address, show extras\n");
	wipe_all();
	run("enable");
	run("debug register write 0x1250 00003fff");
	run("debug register read 1250");
	CHECK(out_has("1250: 00003fff"), "debug register write + read through the mock");
	run("debug serdes write 1 21 3 0xbeef");
	CHECK(last_sds_id == 1 && last_sds_page == 0x21 && last_sds_reg == 3 && last_sds_val == 0xbeef,
	      "debug serdes write: decimal id, hex page/reg/value");
	run("debug register read 12g4");
	CHECK(out_has("'^' marker"), "a non-hex address is rejected");
	run("debug xram test 100 10");
	CHECK(out_has("Refusing"), "xram test refuses the live region");
	run("disable");
	run("debug register read 1250");
	CHECK(out_has("'^' marker"), "debug is privileged");

	run("enable");
	to_if("ethernet 1/4");
	run("speed 100");
	run("duplex half");
	CHECK(sw_ports[3].duplex == PHY_DUPLEX_HALF && last_speed == PHY_SPEED_100M, "duplex half at 100");
	run("speed 10");
	CHECK(sw_ports[3].duplex == PHY_DUPLEX_HALF, "speed keeps the configured duplex");
	render_into(render_a);
	CHECK(strstr(render_a, " speed 10\n duplex half\n") != NULL, "duplex in the running config");
	run("no duplex");
	CHECK(sw_ports[3].duplex == PHY_DUPLEX_BOTH, "no duplex -> auto");

	run("interface vlan 1");
	run("mac-address 0012.3456.789a");
	CHECK(uip_ethaddr.addr[0] == 0x00 && uip_ethaddr.addr[5] == 0x9a, "mac-address, dotted form");
	render_into(render_a);
	CHECK(strstr(render_a, " mac-address 0012.3456.789a\n") != NULL, "rendered while not the boot MAC");
	run("mac-address 02:11:22:33:44:55");
	CHECK(out_has("globally administered") && uip_ethaddr.addr[5] == 0x9a, "locally administered refused");
	run("mac-address 00:11:22:33:44");
	CHECK(out_has("% Invalid MAC address"), "short MAC refused");
	run("no mac-address");
	CHECK(memcmp(uip_ethaddr.addr, sw_mac_boot, 6) == 0, "no mac-address restores the boot MAC");
	render_into(render_a);
	CHECK(!strstr(render_a, "mac-address"), "boot MAC is not rendered");

	run("end");
	run("show logging");
	CHECK(out_has("Remote syslog: off"), "show logging");
	run("show ip igmp snooping");
	CHECK(n_igmp_show == 1, "show ip igmp snooping");
	run("show history");
	CHECK(!out_has("%"), "show history");
}

static void test_interface_range(void)
{
	printf("[test] interface range\n");
	reset_all();
	run("enable");
	to_if("range ethernet 1/2-4,1/7");
	CHECK(cli.mode == CLI_MODE_IF && cli.ctx_range == ((1 << 2) | (1 << 3) | (1 << 4) | (1 << 7)),
	      "interface range takes a list with a span");
	CHECK(cli.ctx_if == 2, "the context starts at the first port of the range");
	out_reset();
	cli_prompt();
	CHECK(out_has("(config-if-range)# "), "range prompt");
	run("switchport access vlan 30");
	CHECK(vl_member(30, 1) && vl_member(30, 2) && vl_member(30, 3) && vl_member(30, 6),
	      "a command applies to every port of the range");
	CHECK(!vl_member(30, 0) && !vl_member(30, 4) && !vl_member(30, 5),
	      "and to no other port");
	run("description lab");
	CHECK(strcmp(port_names[1], "lab") == 0 && strcmp(port_names[6], "lab") == 0
	      && port_names[4][0] == 0, "description on each port");
	run("vlan 40");
	CHECK(cli.mode == CLI_MODE_VLAN && sw_vlan_exists(40),
	      "a global command runs once and leaves the range");

	to_if("ethernet 1/5-6");
	CHECK(cli.ctx_range == ((1 << 5) | (1 << 6)), "NX-OS form: interface ethernet 1/5-6");
	run("exit");
	CHECK(cli.mode == CLI_MODE_CONFIG, "exit leaves the range");
	to_if("e1/1,e1/9");
	CHECK(cli.ctx_range == ((1 << 1) | (1 << 9)), "per-item ethernet prefix");
	to_if("ethernet 1/8");
	CHECK(cli.ctx_range == 0 && cli.ctx_if == 8, "a single port is not a range");
	to_if("ethernet 1/1-1");
	CHECK(cli.ctx_range == 0 && cli.ctx_if == 1, "a one-port span is a single port");

	to_if("ethernet 1/4-2");
	CHECK(out_has("% Invalid input"), "a reversed span is rejected");
	to_if("ethernet 1/1-10");
	CHECK(out_has("% Invalid input"), "a port past 9 is rejected");
	to_if("ethernet 1/1,");
	CHECK(out_has("% Invalid input"), "a trailing comma is rejected");
	to_if("ethernet 1/1-2-3");
	CHECK(out_has("% Invalid input"), "a double span is rejected");
	run("monitor session 1 source interface ethernet 1/1-2");
	CHECK(out_has("% Invalid input"), "single-port arguments still refuse a list");

	replay_text("interface range ethernet 1/2-3\n switchport mode trunk\n"
		    " switchport trunk allowed vlan 10,20\n");
	CHECK(sw_ports[1].mode == sw_ports[2].mode && sw_ports[1].mode != sw_ports[3].mode,
	      "a range replays from a startup config");
}

static void test_l2_extensions(void)
{
	char cfg[CONFIG_LEN];

	printf("[test] tagged-only trunks, per-port STP off, mrouter ports\n");
	wipe_all();
	CHECK(port_ingress_filter_get(0) == VLAN_UNTAGGED && port_ingress_filter_get(8) == VLAN_UNTAGGED,
	      "the access-port default is pushed at init, before any switchport line");
	run("enable");
	run("configure terminal");
	run("vlan 20");
	run("vlan 30");
	to_if("ethernet 1/9");			/* logical 8 */
	run("switchport mode trunk");
	run("switchport trunk allowed vlan 20,30");
	CHECK(port_ingress_filter_get(8) == VLAN_TAGGED,
	      "trunk without its native vlan accepts tagged frames only");
	CHECK(port_pvid_get(8) == 1 && !vl_member(1, 8), "and is no member of the native vlan");
	run("switchport trunk native vlan 20");
	CHECK(port_ingress_filter_get(8) == VLAN_ALL && vl_member(20, 8) && !vl_tagged(20, 8),
	      "an allowed native vlan accepts untagged again");
	run("switchport trunk native vlan 40");
	CHECK(port_ingress_filter_get(8) == VLAN_TAGGED, "native moved out of the list: tagged only");

	to_if("ethernet 1/2");
	run("spanning-tree disable");
	CHECK(STP_PF_OUT(stp_pflags[1]) && (stp_pflags[1] & STP_PF_FILTER),
	      "spanning-tree disable is bpdufilter enable");
	run("ip igmp snooping mrouter");
	CHECK(igmp_mrouter == (1 << 1), "mrouter adds the port to the router mask");
	to_if("ethernet 1/4-5");
	run("ip igmp snooping mrouter");
	CHECK(igmp_mrouter == ((1 << 1) | (1 << 3) | (1 << 4)), "mrouter on a range");
	run("ip igmp snooping");
	CHECK(sw_igmp && cli.mode == CLI_MODE_CONFIG, "global ip igmp snooping still runs from a submode");

	render_into(cfg);
	CHECK(strstr(cfg, "interface ethernet 1/2\n ip igmp snooping mrouter\n spanning-tree bpdufilter enable\n"),
	      "running config shows mrouter and the canonical bpdufilter form");
	CHECK(strstr(cfg, "switchport trunk native vlan 40\n switchport trunk allowed vlan 20,30\n"),
	      "a tagged-only trunk renders as its native and allowed list");

	to_if("ethernet 1/3");			/* logical 2 */
	n_stp_sync = 0;
	run("spanning-tree portfast");
	CHECK(!STP_PF_OUT(stp_pflags[2]) && n_stp_sync == 0, "portfast keeps the port in STP");
	run("spanning-tree bpdufilter enable");
	CHECK(STP_PF_OUT(stp_pflags[2]) && n_stp_sync == 1, "bpdufilter alone takes the port out");
	CHECK(out_has("% Warning: portfast has no effect"), "filtering a portfast port warns");
	run("spanning-tree portfast");
	CHECK(out_has("% Warning: portfast has no effect"), "and so does portfast on a filtered port");
	run("no spanning-tree portfast");
	CHECK(STP_PF_OUT(stp_pflags[2]) && n_stp_sync == 1 && !out_has("Warning"),
	      "portfast makes no difference to that; its no form does not warn");
	run("no spanning-tree disable");
	CHECK(!STP_PF_OUT(stp_pflags[2]) && n_stp_sync == 2, "no spanning-tree disable clears the filter");

	to_if("ethernet 1/2");
	run("no spanning-tree disable");
	run("no ip igmp snooping mrouter");
	CHECK(!STP_PF_OUT(stp_pflags[1]) && igmp_mrouter == ((1 << 3) | (1 << 4)),
	      "no forms restore both");

	wipe_all();
	out_reset();
	replay_text("interface ethernet 1/6\n spanning-tree portfast\n spanning-tree bpdufilter enable\n");
	CHECK(!out_has("Warning"), "the warning stays quiet during the boot replay");
	wipe_all();
	replay_text(cfg);
	CHECK(STP_PF_OUT(stp_pflags[1]) && igmp_mrouter == ((1 << 1) | (1 << 3) | (1 << 4))
	      && port_ingress_filter_get(8) == VLAN_TAGGED, "all three replay from a saved config");
}

static void test_default_boot(void)
{
	printf("[test] default config: dhcp with the built-in address as fallback\n");
	wipe_all();
	replay_text("interface vlan 1\n ip address dhcp\n!\nfeature telnet\n");
	CHECK(n_dhcp_start == 1 && telnet_state.enabled, "dhcp client and telnet start");
	run("show ip interface brief");
	CHECK(out_has("dhcp, no lease yet"), "no lease yet is shown as such");
	dhcp_state.state = DHCP_LEASING;
	run("show ip interface brief");
	CHECK(out_has("  dhcp\n"), "a lease shows plain dhcp");
	dhcp_state.state = DHCP_OFF;
	run("show ip interface brief");
	CHECK(out_has("static"), "without the client: static");
}

/* ---- LACP: a simulated partner ---- */
extern uint8_t uip_buf[];
extern u16_t uip_len;
extern int n_tx_frames;
extern uint8_t tx_frames[16][160];

static void links_up(uint16_t m)
{
	hw_reg_set(RTL837X_REG_LINKS_STS, ((uint32_t)(m & 0xff) << 16) | ((uint32_t)(m >> 8) << 8));
}

/* The last LACPDU sent out of logical port lp, or NULL */
static uint8_t *last_pdu(int lp)
{
	for (int i = n_tx_frames - 1; i >= 0 && i >= n_tx_frames - 16; i--) {
		uint8_t *f = tx_frames[i & 15] + RTL_FRAME_DESC_SIZE;
		if (f[5] == 0x02 && f[20] == 0x88 && f[21] == 0x09 && ((f[18] << 8 | f[19]) == (1 << lp)))
			return f + 22;
	}
	return NULL;
}

/* An LACPDU from the partner arriving on logical port lp: the partner is
 * system 00:aa:00:00:00:<sys> with key `key`, port lp+100; its partner TLV
 * echoes what we last sent on lp when echo is set. */
static void partner_pdu(int lp, int sys, int key, uint8_t st, int echo)
{
	uint8_t *pdu = &uip_buf[26], *ours = last_pdu(lp);
	memset(uip_buf, 0, 160);
	uip_buf[0] = 0x01; uip_buf[1] = 0x80; uip_buf[2] = 0xc2; uip_buf[5] = 0x02;
	uip_buf[12] = 0x88; uip_buf[13] = 0x99;
	uip_buf[19] = lp;
	uip_buf[24] = 0x88; uip_buf[25] = 0x09;
	pdu[0] = 1; pdu[1] = 1;
	pdu[2] = 1; pdu[3] = 20;
	pdu[4] = 0x80; pdu[5] = 0x00;
	pdu[7] = 0xaa; pdu[11] = sys;
	pdu[12] = key >> 8; pdu[13] = key;
	pdu[14] = 0x80; pdu[15] = 0x00;
	pdu[16] = 0; pdu[17] = lp + 100;
	pdu[18] = st;
	pdu[22] = 2; pdu[23] = 20;
	if (echo && ours)
		memcpy(pdu + 24, ours + 4, 15);
	pdu[42] = 3; pdu[43] = 16;
	uip_len = 26 + 110;
	lacp_in();
}

static void lacp_ticks(int n)
{
	while (n--)
		lacp_tick();
}

#define P_ALL (LACP_ST_ACTIVITY | LACP_ST_AGGREGATION | LACP_ST_SYNC | LACP_ST_COLLECTING | LACP_ST_DISTRIBUTING)

static void test_lacp(void)
{
	char cfg[CONFIG_LEN];
	uint8_t *pdu;

	printf("[test] LACP\n");
	wipe_all();
	links_up(0x1ff);
	run("enable");
	to_if("ethernet 1/5-6");			/* logical 4, 5 */
	run("channel-group 1 mode active");
	CHECK(lacp_ports == 0x30 && lacp_group[4] == 1 && lacp_mode[5] == LACP_MODE_ACTIVE,
	      "channel-group N mode active makes LACP ports");
	CHECK(port_lag_members_get(0) == 0, "nothing is bundled before a partner answers");
	CHECK(hw_reg_get(RTL837X_RMA_CTRL(2)) == RMA_ACT_FORWARD, "LACPDUs are no longer discarded by the ASIC");
	n_tx_frames = 0;
	lacp_tick();
	pdu = last_pdu(4);
	CHECK(pdu && last_pdu(5), "an active port sends an LACPDU at once");
	CHECK(pdu && pdu[0] == 1 && pdu[2] == 1 && pdu[3] == 20 && pdu[22] == 2 && pdu[42] == 3,
	      "subtype, actor, partner and collector TLVs where the standard puts them");
	CHECK(pdu && (pdu[18] & (LACP_ST_ACTIVITY | LACP_ST_AGGREGATION | LACP_ST_DEFAULTED))
	      == (LACP_ST_ACTIVITY | LACP_ST_AGGREGATION | LACP_ST_DEFAULTED) && !(pdu[18] & LACP_ST_SYNC),
	      "actor state: active, aggregatable, defaulted, not in sync");
	CHECK(pdu && pdu[12] == 0 && pdu[13] == 1 && pdu[17] == 5 && !memcmp(pdu + 6, uip_ethaddr.addr, 6),
	      "actor key is the port-channel, port number and system MAC");
	CHECK(tx_frames[0][RTL_FRAME_DESC_SIZE + 12] == 0x88 && tx_frames[0][RTL_FRAME_DESC_SIZE + 13] == 0x99,
	      "sent with a CPU tag");

	/* the partner answers without having heard us yet, then in sync */
	partner_pdu(4, 1, 7, LACP_ST_ACTIVITY | LACP_ST_AGGREGATION, 0);
	CHECK(port_lag_members_get(0) == 0, "an unmatched partner does not bundle");
	lacp_tick();
	partner_pdu(4, 1, 7, P_ALL, 1);
	CHECK(port_lag_members_get(0) == (1 << 4), "matched and in sync on both ends: bundled");
	lacp_tick();
	pdu = last_pdu(4);
	CHECK(pdu && (pdu[18] & (LACP_ST_SYNC | LACP_ST_COLLECTING | LACP_ST_DISTRIBUTING))
	      == (LACP_ST_SYNC | LACP_ST_COLLECTING | LACP_ST_DISTRIBUTING) && !(pdu[18] & LACP_ST_DEFAULTED),
	      "and says so: sync, collecting, distributing");
	CHECK(pdu && pdu[27] == 0xaa && pdu[31] == 1 && pdu[32] == 0 && pdu[33] == 7,
	      "the partner TLV names the partner");
	partner_pdu(5, 1, 7, P_ALL, 1);
	lacp_tick();
	partner_pdu(5, 1, 7, P_ALL, 1);
	CHECK(port_lag_members_get(0) == 0x30, "the second port joins the same partner");

	run("show port-channel summary");
	CHECK(out_has("LACP") && out_has("Eth1/5(P)") && out_has("Eth1/6(P)"), "show port-channel summary");
	run("show lacp neighbor");
	CHECK(out_has("bundled") && out_has("32768,00aa.0000.0001") && out_has("Po1"), "show lacp neighbor");

	/* another system on port 6 */
	partner_pdu(5, 2, 7, P_ALL, 1);
	CHECK(port_lag_members_get(0) == (1 << 4), "a port facing another system leaves the bundle");
	run("show lacp");
	CHECK(out_has("other system"), "and show lacp says why");

	/* link down */
	links_up(0x1ff & ~(1 << 4));
	lacp_tick();
	CHECK(!(port_lag_members_get(0) & (1 << 4)), "link down leaves the bundle");
	links_up(0x1ff);
	partner_pdu(5, 1, 7, P_ALL, 1);
	lacp_tick();
	partner_pdu(5, 1, 7, P_ALL, 1);
	CHECK(port_lag_members_get(0) == (1 << 5), "port 6 now defines the partner and bundles");

	/* timeout: long timeout is 90 s */
	lacp_ticks(89 * LACP_TICK_HZ);
	CHECK(port_lag_members_get(0) == (1 << 5), "the partner info holds for the long timeout");
	lacp_ticks(2 * LACP_TICK_HZ);
	CHECK(port_lag_members_get(0) == 0, "and expires after it");

	/* fast rate: short timeout */
	to_if("ethernet 1/6");
	run("lacp rate fast");
	partner_pdu(5, 1, 7, P_ALL, 1);
	lacp_tick();
	partner_pdu(5, 1, 7, P_ALL, 1);
	CHECK(port_lag_members_get(0) == (1 << 5), "bundled again");
	lacp_tick();
	pdu = last_pdu(5);
	CHECK(pdu && (pdu[18] & LACP_ST_TIMEOUT), "lacp rate fast asks for the short timeout");
	lacp_ticks(4 * LACP_TICK_HZ);
	CHECK(port_lag_members_get(0) == 0, "and times out after 3 s");

	/* min-links */
	run("interface port-channel 1");
	run("lacp min-links 2");
	partner_pdu(4, 1, 7, P_ALL, 1);
	lacp_tick();
	partner_pdu(4, 1, 7, P_ALL, 1);
	CHECK(port_lag_members_get(0) == 0, "min-links 2 keeps a single ready port out");
	partner_pdu(5, 1, 7, P_ALL, 1);
	lacp_tick();
	partner_pdu(5, 1, 7, P_ALL, 1);
	CHECK(port_lag_members_get(0) == 0x30, "two ready ports bundle");

	/* mixing static and LACP members */
	to_if("ethernet 1/7");
	run("channel-group 1 mode on");
	CHECK(out_has("has LACP members") && port_lag_of(6) == PORT_LAG_NONE, "static into an LACP group refused");
	run("channel-group 2 mode on");
	to_if("ethernet 1/8");
	run("channel-group 2 mode active");
	CHECK(out_has("has static members") && !lacp_group[7], "LACP into a static group refused");

	/* passive */
	run("channel-group 3 mode passive");
	n_tx_frames = 0;
	lacp_ticks(5);
	CHECK(!last_pdu(7), "a passive port stays quiet");
	partner_pdu(7, 3, 9, LACP_ST_ACTIVITY | LACP_ST_AGGREGATION, 0);
	lacp_tick();
	CHECK(last_pdu(7) != NULL, "and answers an active partner");

	/* globals and rendering */
	run("lacp system-priority 100");
	run("port-channel load-balance src-dst-ip");
	CHECK(lacp_sysprio == 100, "lacp system-priority");
	render_into(cfg);
	CHECK(strstr(cfg, "lacp system-priority 100\n") && strstr(cfg, "port-channel load-balance src-dst-ip\n"),
	      "globals render");
	CHECK(strstr(cfg, "interface port-channel 1\n lacp min-links 2\n")
	      && strstr(cfg, "interface port-channel 3\n"), "port-channels with LACP ports render, bundled or not");
	CHECK(strstr(cfg, "interface ethernet 1/6\n lacp rate fast\n channel-group 1 mode active\n")
	      && strstr(cfg, "interface ethernet 1/8\n channel-group 3 mode passive\n")
	      && strstr(cfg, "interface ethernet 1/7\n channel-group 2 mode on\n"), "members render with their mode");
	CHECK(!strstr(cfg, "\n load-balance"), "the global hash is not repeated per port-channel");

	wipe_all();
	links_up(0x1ff);
	out_reset();
	replay_text(cfg);
	CHECK(!out_has("% "), "the LACP configuration replays without errors");
	CHECK(lacp_group[4] == 1 && lacp_group[5] == 1 && lacp_fast[5] && lacp_mode[7] == LACP_MODE_PASSIVE
	      && lacp_minlinks[0] == 2 && lacp_sysprio == 100 && port_lag_of(6) == 1,
	      "and restores it");

	run("enable");
	to_if("ethernet 1/5");
	run("no channel-group");
	CHECK(!lacp_group[4] && lacp_ports == ((1 << 5) | (1 << 7)), "no channel-group leaves LACP");
	run("channel-group 4");
	CHECK(port_lag_of(4) == 3 && !lacp_group[4], "a bare channel-group N is still static");
	/* a port-channel whose LACP ports never bundled keeps the chip's
	 * pristine hash; it must not render as a load-balance setting */
	wipe_all();
	for (int g = 0; g < 4; g++)
		hw_reg_set(RTL837X_TRK_HASH_CTRL_BASE + (g << 2), LAG_HASH_RESET);
	run("enable");
	to_if("ethernet 1/3");
	run("channel-group 3 mode active");
	render_into(cfg);
	CHECK(strstr(cfg, "interface port-channel 3\n!\n") && !strstr(cfg, "load-balance"),
	      "a pristine hash renders as the default");
	run("show port-channel summary");
	CHECK(out_has("src-mac dst-mac src-ip dst-ip l4-src-port l4-dst-port"), "and shows as the default");
	run("no channel-group");

	run("enable");
	to_if("ethernet 1/6");
	run("no channel-group");
	to_if("ethernet 1/8");
	run("no channel-group");
	CHECK(!lacp_ports && hw_reg_get(RTL837X_RMA_CTRL(2)) == RMA_ACT_DISCARD,
	      "without LACP ports the ASIC discards LACPDUs again");
}

static void test_dns(void)
{
	char cfg[CONFIG_LEN];
	extern int n_dns_lookup, n_dns_show;

	printf("[test] DNS resolver settings\n");
	wipe_all();
	memset(&dns_state, 0, sizeof(dns_state));
	run("enable");
	run("configure terminal");
	run("ip name-server 192.168.0.1 9.9.9.9");
	CHECK(dns_state.server[0][0] == 192 && dns_state.server[0][3] == 1 && dns_state.server[1][0] == 9,
	      "ip name-server takes two servers");
	render_into(cfg);
	CHECK(strstr(cfg, "ip name-server 192.168.0.1 9.9.9.9\n") != NULL, "and renders them");
	run("ip name-server 1.1.1.1");
	CHECK(dns_state.server[0][0] == 1 && !dns_state.server[1][0], "one server replaces both");
	run("no ip name-server");
	CHECK(!dns_state.server[0][0], "no ip name-server clears them");
	render_into(cfg);
	CHECK(!strstr(cfg, "name-server"), "and nothing renders");
	run("end");
	n_dns_lookup = 0;
	run("nslookup pool.ntp.org");
	CHECK(n_dns_lookup == 1 && !strcmp(dns_state.name, "pool.ntp.org") && out_has("show hosts"),
	      "nslookup starts a lookup");
	run("nslookup other.example");
	CHECK(out_has("already running") && n_dns_lookup == 1, "one lookup at a time");
	run("show hosts");
	CHECK(n_dns_show == 1, "show hosts");
	dns_state.status = DNS_IDLE;
}

static void test_ntp(void)
{
	char cfg[CONFIG_LEN];
	extern int n_ntp_start, n_ntp_stop;

	printf("[test] NTP client and clock settings\n");
	wipe_all();
	memset(&ntp_state, 0, sizeof(ntp_state));
	strcpy(ntp_state.tz_name, "UTC");
	run("enable");
	run("configure terminal");
	render_into(cfg);
	CHECK(!strstr(cfg, "clock ") && !strstr(cfg, "ntp "), "defaults render nothing");
	run("ntp server pool.ntp.org");
	CHECK(n_ntp_start == 1 && !strcmp(ntp_state.server, "pool.ntp.org"), "ntp server starts the client");
	run("clock timezone EST -5 0");
	CHECK(ntp_state.offset == -300 && !strcmp(ntp_state.tz_name, "EST"), "clock timezone EST -5 0");
	run("clock timezone IST 5 30");
	CHECK(ntp_state.offset == 330, "half-hour zones");
	run("clock timezone X 5 20");
	CHECK(out_has("% Value out of range") && ntp_state.offset == 330, "odd minutes refused");
	run("clock timezone X -13");
	CHECK(out_has("% Value out of range"), "beyond -12 refused");
	run("clock timezone TOOLONGNAME 1");
	CHECK(out_has("Invalid zone name"), "long names refused");
	run("clock timezone CET 1");
	run("clock summer-time CEST recurring eu");
	CHECK(ntp_state.offset == 60 && ntp_state.dst == NTP_DST_EU && !strcmp(ntp_state.dst_name, "CEST"),
	      "EU summer time");
	render_into(cfg);
	CHECK(strstr(cfg, "clock timezone CET 1 0\nclock summer-time CEST recurring eu\nntp server pool.ntp.org\n") != NULL,
	      "clock and ntp render");
	run("clock timezone EST -5 0");
	run("clock summer-time EDT recurring");
	CHECK(ntp_state.dst == NTP_DST_US, "plain recurring is the US rule");
	render_into(cfg);
	CHECK(strstr(cfg, "clock timezone EST -5 0\nclock summer-time EDT recurring\n") != NULL, "negative offsets render");

	wipe_all();
	memset(&ntp_state, 0, sizeof(ntp_state));
	strcpy(ntp_state.tz_name, "UTC");
	out_reset();
	replay_text(cfg);
	CHECK(!out_has("% ") && ntp_state.offset == -300 && ntp_state.dst == NTP_DST_US
	      && !strcmp(ntp_state.server, "pool.ntp.org"), "and replay");
	run("enable");
	run("configure terminal");
	run("no ntp server");
	run("no clock summer-time");
	run("no clock timezone");
	CHECK(n_ntp_stop >= 1 && !ntp_state.server[0] && ntp_state.dst == NTP_DST_OFF && !ntp_state.offset
	      && !strcmp(ntp_state.tz_name, "UTC"), "no forms restore the defaults");
	run("end");
	run("show clock");
	CHECK(out_has("CLOCK"), "show clock");
	run("show ntp status");
	CHECK(out_has("NTP"), "show ntp status");
}

static void test_totp_cfg(void)
{
	char cfg[CONFIG_LEN];

	printf("[test] TOTP login settings\n");
	wipe_all();
	totp_enabled = totp_keylen = 0;
	totp_b32[0] = 0;
	run("enable");
	run("configure terminal");
	run("line vty");
	run("login totp");
	CHECK(out_has("Set a secret first") && !totp_enabled, "login totp needs a secret");
	run("totp secret GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ");
	CHECK(totp_keylen && !strcmp(totp_b32, "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ") && out_has("otpauth://totp/"),
	      "totp secret sets the key and prints the authenticator URI");
	run("totp secret SHORT");
	CHECK(out_has("Invalid base32 secret") && !strcmp(totp_b32, "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"),
	      "a bad secret is refused and the old one kept");
	run("login totp");
	CHECK(totp_enabled && out_has("not synchronised"), "login totp warns without a clock");
	render_into(cfg);
	CHECK(strstr(cfg, "line vty\n totp secret GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\n login totp\n") != NULL,
	      "secret and login totp render under line vty");
	wipe_all();
	totp_enabled = totp_keylen = 0;
	totp_b32[0] = 0;
	out_reset();
	replay_text(cfg);
	CHECK(!out_has("% ") && !out_has("otpauth") && totp_enabled && totp_keylen, "and replay quietly");
	run("enable");
	run("configure terminal");
	run("line vty");
	run("no totp secret");
	CHECK(!totp_enabled && !totp_keylen && !totp_b32[0], "no totp secret turns the factor off");
	run("end");
	run("show totp");
	CHECK(out_has("TOTP"), "show totp");
	run("disable");
	run("show totp");
	CHECK(!out_has("TOTP"), "show totp needs privileged EXEC");
}

static void test_copy_forms(void)
{
	printf("[test] copy run start and copy start run\n");
	wipe_all();
	run("enable");
	run("copy run start");
	CHECK(saved_ok(), "copy run start saves");
	run("configure terminal");
	run("hostname merged");
	run("end");
	run("copy running-config startup-config");
	CHECK(saved_ok() && strstr((char *)fake_cfg, "hostname merged\n"), "the full spelling saves too");
	run("configure terminal");
	run("hostname changed");
	run("vlan 77");
	run("end");
	run("copy start run");
	CHECK(!strcmp(hostname, "merged") && cli.mode == CLI_MODE_PRIV, "copy start run merges the startup config");
	CHECK(sw_vlan_exists(77), "a merge adds; it does not remove what the startup config lacks");
	run("configure terminal");
	run("hostname again");
	run("copy startup-config running-config");
	CHECK(!strcmp(hostname, "merged") && cli.mode == CLI_MODE_CONFIG,
	      "full spelling from config mode, which stays config mode");
}

static void test_show_run_filters(void)
{
	printf("[test] show running-config interface / vlan\n");
	wipe_all();
	run("enable");
	run("configure terminal");
	run("vlan 10");
	run("name home");
	run("vlan 20");
	run("interface ethernet 1/3");
	run("description desk");
	run("switchport access vlan 10");
	run("interface ethernet 1/5-6");
	run("channel-group 2 mode on");
	run("interface vlan 1");
	run("ip address 10.0.0.2 255.255.255.0");
	run("end");

	run("show running-config interface ethernet 1/3");
	CHECK(out_has("interface ethernet 1/3\n description desk\n switchport access vlan 10\n!\n"),
	      "one interface block");
	CHECK(!out_has("hostname") && !out_has("ethernet 1/4") && !out_has("vlan 10\n name")
	      && !out_has("feature") && !out_has("end"), "and nothing else");
	run("show run int eth1/3");
	CHECK(out_has("interface ethernet 1/3\n description desk") && !out_has("1/4"), "abbreviated, NX-OS spelling");
	run("show run int 1/1-3");
	CHECK(out_has("ethernet 1/1\n") && out_has("ethernet 1/2\n") && out_has("ethernet 1/3\n")
	      && !out_has("ethernet 1/4\n"), "a list");
	run("show running-config interface port-channel 2");
	CHECK(out_has("interface port-channel 2\n") && !out_has("ethernet 1/5"), "a port-channel");
	run("show running-config interface vlan 1");
	CHECK(out_has("interface vlan 1\n ip address 10.0.0.2 255.255.255.0\n") && !out_has("ethernet"),
	      "the management interface");
	run("show running-config interface");
	CHECK(out_has("interface port-channel 2") && out_has("interface ethernet 1/9") && out_has("interface vlan 1")
	      && !out_has("hostname") && !out_has("\nvlan 10\n"), "every interface block");
	run("show running-config vlan");
	CHECK(out_has("vlan 10\n name home\n") && out_has("vlan 20\n") && !out_has("interface"), "the VLAN blocks");
	run("show running-config vlan 20");
	CHECK(out_has("vlan 20\n") && !out_has("vlan 10"), "one VLAN");
	run("show running-config");
	CHECK(out_has("hostname") && out_has("interface ethernet 1/3") && out_has("end"),
	      "the unfiltered form is unchanged afterwards");
	run("write memory");
	CHECK(saved_ok() && strstr((char *)fake_cfg, "hostname"), "and saving never filters");
}

static void test_show_if_detail(void)
{
	printf("[test] show interfaces ethernet LIST\n");
	wipe_all();
	run("enable");
	run("configure terminal");
	run("interface ethernet 1/3");
	run("description desk");
	run("switchport access vlan 10");
	run("mtu 9000");
	run("rate-limit input 1024 drop");
	run("interface ethernet 1/9");
	run("switchport mode trunk");
	run("switchport trunk allowed vlan 10");
	run("end");
	run("show interfaces ethernet 1/3");
	CHECK(out_has("Ethernet1/3 is ") && out_has("  Description: desk\n") && out_has("  MTU: 9000 bytes\n")
	      && out_has("  Switchport: access, VLAN 10\n") && out_has("input 1024 kbit/s (drop)")
	      && out_has("  Input: ") && !out_has("Ethernet1/4"), "one port in detail");
	run("show int eth1/9");
	CHECK(out_has("Ethernet1/9") && out_has("trunk, native VLAN 1, allowed 10 (tagged frames only)")
	      , "NX-OS spelling, a trunk without its native VLAN");
	run("show int 1/1-2");
	CHECK(out_has("Ethernet1/1 ") && out_has("Ethernet1/2 ") && !out_has("Ethernet1/3"), "a list");
	run("show interfaces status");
	CHECK(out_has("Port      Name"), "the table views are unchanged");
}

static void test_ping_cli(void)
{
	extern char ping_host[];
	extern uint16_t ping_count, ping_size;

	printf("[test] ping command line\n");
	wipe_all();
	run("enable");
	run("ping 192.168.0.1");
	CHECK(!strcmp(ping_host, "192.168.0.1") && ping_count == 5 && ping_size == 100, "defaults: 5 echos of 100 bytes");
	run("ping pool.ntp.org repeat 3");
	CHECK(!strcmp(ping_host, "pool.ntp.org") && ping_count == 3 && ping_size == 100, "repeat");
	run("ping 10.0.0.1 size 1500");
	CHECK(ping_count == 5 && ping_size == 1500, "size");
	run("ping 10.0.0.1 repeat 2 size 64");
	CHECK(ping_count == 2 && ping_size == 64, "both");
	run("ping 10.0.0.1 size 20");
	CHECK(out_has("% Invalid input"), "size below 36 refused");
	run("disable");
	run("ping 10.0.0.1");
	CHECK(out_has("% Invalid input"), "ping needs privileged EXEC");
}

static void test_log(void)
{
	printf("[test] local log buffer\n");
	wipe_all();
	log_clear();
	run("enable");
	run("write memory");
	run("show logging");
	CHECK(out_has("Log buffer (2048 bytes):") && out_has("%SYS-5-CONFIG_I: Configuration saved to startup-config\n"),
	      "write memory is logged and shown");
	CHECK(out_has("*00:00:"), "uptime stamp while the clock is not set");
	run("clear logging");
	run("show logging");
	CHECK(!out_has("CONFIG_I"), "clear logging empties it");
	for (int i = 0; i < 120; i++) {
		log_begin("TEST-5-FILL");
		log_s("entry ");
		log_dec(i);
		log_end();
	}
	run("show logging");
	CHECK(log_wrapped && out_has("entry 119\n") && !out_has("entry 0\n")
	      && strstr(out_buf, "\n*") != NULL, "after a wrap: newest kept, oldest dropped, whole lines only");
	CHECK(!strstr(out_buf, "bytes):\n\n%") && !strstr(out_buf, "bytes):\n\nTEST"), "no partial first line");
	log_clear();
}

/* An LLDPDU from a neighbour, as the CPU receives it on logical port lp */
static void lldp_rx(int lp)
{
	static const uint8_t tlv[] = {
		0x02, 0x07, 0x04, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55,	/* chassis: MAC */
		0x04, 0x07, 0x05, 'g', 'i', '1', '/', '2', '4',		/* port: name */
		0x06, 0x02, 0x00, 0x78,					/* TTL 120 */
		0x08, 0x06, 'u', 'p', 'l', 'i', 'n', 'k',		/* port description */
		0x0a, 0x04, 'c', 'o', 'r', 'e',				/* system name */
		0x0e, 0x04, 0x00, 0x14, 0x00, 0x14,			/* caps: bridge, router */
		0x10, 0x0c, 0x05, 0x01, 10, 0, 0, 1, 0x01, 0, 0, 0, 0, 0,	/* mgmt 10.0.0.1 */
		0x00, 0x00 };
	memset(uip_buf, 0, 200);
	uip_buf[0] = 0x01; uip_buf[1] = 0x80; uip_buf[2] = 0xc2; uip_buf[5] = 0x0e;
	uip_buf[19] = lp;
	uip_buf[24] = 0x88; uip_buf[25] = 0xcc;
	memcpy(&uip_buf[26], tlv, sizeof(tlv));
	uip_len = 26 + sizeof(tlv);
	lldp_in();
}

static void test_lldp(void)
{
	char cfg[CONFIG_LEN];
	uint8_t *f;

	printf("[test] LLDP\n");
	wipe_all();
	links_up(1 << 8);			/* port 1/9 up */
	run("enable");
	run("show lldp neighbors");
	CHECK(out_has("LLDP is not enabled"), "off by default");
	run("configure terminal");
	run("feature lldp");
	CHECK(lldp_enabled, "feature lldp");
	n_tx_frames = 0;
	lldp_tick();
	f = tx_frames[0] + RTL_FRAME_DESC_SIZE;
	CHECK(n_tx_frames == 1 && f[5] == 0x0e && f[20] == 0x88 && f[21] == 0xcc && f[19] == (1 << 8 & 0xff) + 0
	      && f[18] == 0x01, "an LLDPDU out of the one port that is up");
	CHECK(f[22] == 0x02 && f[23] == 7 && f[24] == 4 && !memcmp(f + 25, uip_ethaddr.addr, 6),
	      "chassis ID TLV: the MAC");
	CHECK(f[31] == 0x04 && f[33] == 5 && !memcmp(f + 34, "Ethernet1/9", 11), "port ID TLV: the interface name");
	lldp_tick();
	CHECK(n_tx_frames == 1, "not again until the interval is over");
	lldp_rx(8);
	run("show lldp neighbors");
	CHECK(out_has("core") && out_has("Eth1/9") && out_has("120") && out_has("B,R") && out_has("gi1/24")
	      && out_has("Total entries displayed: 1"), "the neighbour in the table");
	run("show lldp neighbors detail");
	CHECK(out_has("Chassis ID: 0011.2233.4455") && out_has("Port description: uplink")
	      && out_has("Management address: 10.0.0.1"), "and in detail");
	for (int i = 0; i < 121; i++)
		lldp_tick();
	run("show lldp neighbors");
	CHECK(out_has("Total entries displayed: 0"), "forgotten after the hold time");
	run("interface ethernet 1/9");
	run("no lldp receive");
	lldp_rx(8);
	CHECK(!lldp_nb[8].ttl, "no lldp receive ignores it");
	run("no lldp transmit");
	n_tx_frames = 0;
	for (int i = 0; i < 31; i++)
		lldp_tick();
	CHECK(n_tx_frames == 0, "no lldp transmit stays quiet");
	render_into(cfg);
	CHECK(strstr(cfg, "feature lldp\n") && strstr(cfg, " no lldp transmit\n no lldp receive\n"), "renders");
	wipe_all();
	out_reset();
	replay_text(cfg);
	CHECK(!out_has("% ") && lldp_enabled && (lldp_no_tx & (1 << 8)), "and replays");
	run("enable");
	run("configure terminal");
	run("no lldp run");
	CHECK(!lldp_enabled, "no lldp run turns it off");
}

static void test_clear_counters(void)
{
	printf("[test] clear counters\n");
	wipe_all();
	run("enable");
	hw_counter_set(2, STAT_COUNTER_RX_PKTS, 1000);
	hw_counter_set(3, STAT_COUNTER_RX_PKTS, 500);
	run("show int eth1/3");
	CHECK(out_has("Input: 1000 packets"), "counters before");
	run("clear counters interface ethernet 1/3");
	hw_counter_set(2, STAT_COUNTER_RX_PKTS, 1007);
	run("show int eth1/3");
	CHECK(out_has("Input: 7 packets"), "counted from the clear");
	run("show int eth1/4");
	CHECK(out_has("Input: 500 packets"), "other ports untouched");
	run("clear counters");
	run("show int eth1/4");
	CHECK(out_has("Input: 0 packets"), "clear counters clears all");
	sw_counters_clear(0xffff);
	hw_counter_set(2, STAT_COUNTER_RX_PKTS, 0);
	hw_counter_set(3, STAT_COUNTER_RX_PKTS, 0);
	sw_counters_clear(0xffff);
}

static void test_errdisable(void)
{
	char cfg[CONFIG_LEN];

	printf("[test] errdisable recovery and err-disabled ports\n");
	wipe_all();
	run("enable");
	run("show interfaces status err-disabled");
	CHECK(out_has("No err-disabled ports") && out_has("Recovery: off"), "none by default");
	stp_pflags[3] |= STP_PF_TRIPPED;	/* BPDU guard tripped on 1/4 */
	run("show interfaces status");
	CHECK(out_has("err-disabled"), "status shows err-disabled");
	run("show interfaces status err-disabled");
	CHECK(out_has("Eth1/4") && out_has("bpduguard"), "the err-disabled list");
	run("configure terminal");
	run("errdisable recovery cause bpduguard");
	run("errdisable recovery interval 60");
	CHECK(stp_errdis_on && stp_errdis_int == 60, "recovery settings");
	run("errdisable recovery interval 5");
	CHECK(out_has("% Value out of range") && stp_errdis_int == 60, "at least 30 s");
	render_into(cfg);
	CHECK(strstr(cfg, "errdisable recovery cause bpduguard\nerrdisable recovery interval 60\n") != NULL, "renders");
	run("interface ethernet 1/4");
	run("no shutdown");
	CHECK(!(stp_pflags[3] & STP_PF_TRIPPED), "no shutdown clears the trip");
	run("exit");
	run("no errdisable recovery cause bpduguard");
	run("no errdisable recovery interval");
	CHECK(!stp_errdis_on && stp_errdis_int == 300, "no forms restore the defaults");
}

static void test_ios_compat(void)
{
	extern uint8_t last_tftp_op, last_tftp_srv[4];
	extern char last_tftp_file[];
	char cfg[CONFIG_LEN];

	printf("[test] IOS compatibility: do, ignored lines, aliases\n");
	wipe_all();
	run("enable");
	run("configure terminal");
	run("do show clock");
	CHECK(out_has("CLOCK"), "do COMMAND in config mode");
	out_reset();
	replay_text("Building configuration...\nCurrent configuration : 1234 bytes\n!\nversion 15.2\n"
		    "service timestamps debug datetime msec\nno service pad\nboot-start-marker\nboot-end-marker\n"
		    "no ip domain-lookup\nvtp mode transparent\nno cdp run\nspanning-tree extend system-id\n"
		    "spanning-tree mode rapid-pvst\nspanning-tree vlan 1-4094 priority 8192\n"
		    "ip route 0.0.0.0 0.0.0.0 10.0.0.254\n"
		    "interface GigabitEthernet1/0/3\n switchport trunk encapsulation dot1q\n switchport mode trunk\n"
		    " switchport nonegotiate\n description gi-port\n!\ninterface Te1/0/9\n description ten\n");
	CHECK(!out_has("% "), "a pasted IOS configuration replays without errors");
	CHECK(stp_rstp == 1 && stp_prio == 0x20, "rapid-pvst is rstp, the VLAN priority is the bridge priority");
	CHECK(((uint8_t *)uip_draddr)[0] == 10 && ((uint8_t *)uip_draddr)[3] == 254, "the default route is the gateway");
	CHECK(!strcmp(port_names[2], "gi-port") && sw_ports[2].mode == SW_MODE_TRUNK
	      && !strcmp(port_names[8], "ten"), "GigabitEthernet1/0/3 and Te1/0/9 are ports 1/3 and 1/9");
	render_into(cfg);
	CHECK(!strstr(cfg, "version") && !strstr(cfg, "vtp") && !strstr(cfg, "nonegotiate"), "nothing ignored renders");
	run("enable");
	run("configure terminal");
	run("ip route 10.0.0.0 255.0.0.0 10.0.0.1");
	CHECK(out_has("Only the default route"), "other routes are refused");
	run("spanning-tree mode pvst");
	CHECK(stp_rstp == 0, "pvst is stp");
	run("end");
	run("terminal length 0");
	CHECK(!out_has("% "), "terminal length 0");
	run("show run int GigabitEthernet1/0/3");
	CHECK(out_has("interface ethernet 1/3"), "IOS names in show commands too");
	run("copy tftp://192.168.0.27/fw.bin flash:");
	CHECK(last_tftp_op == TFTP_OP_GET_FW && last_tftp_srv[0] == 192 && last_tftp_srv[3] == 27
	      && !strcmp(last_tftp_file, "fw.bin"), "copy tftp://host/file flash:");
	run("copy startup-config tftp://10.1.2.3/sw.cfg");
	CHECK(last_tftp_op == TFTP_OP_PUT_CONFIG && last_tftp_srv[3] == 3 && !strcmp(last_tftp_file, "sw.cfg"),
	      "copy startup-config tftp://host/file");
	run("copy http://x/y flash:");
	CHECK(out_has("Only tftp://"), "other schemes refused");
}

int main(void)
{
	printf("== cli.c modal engine tests ==\n");
	test_modes();
	test_abbreviation();
	test_errors();
	test_priv_gating();
	test_old_syntax();
	test_exec_anywhere();
	test_write_and_copy();
	test_help();
	test_complete();
	test_no_prefix();
	test_interface_config();
	test_vlan_db();
	test_switchport_access();
	test_switchport_trunk();
	test_range_math();
	test_global_config();
	test_no_edge_cases();
	test_block_replay();
	test_runcfg_defaults();
	test_runcfg_roundtrip();
	test_write_and_startup();
	test_replay_legacy_and_comments();
	test_physical_shadows();
	test_port_features();
	test_monitor();
	test_port_channel();
	test_stp();
	test_roundtrip_all();
	test_sessions();
	test_show();
	test_step4();
	test_interface_range();
	test_l2_extensions();
	test_default_boot();
	test_lacp();
	test_dns();
	test_ntp();
	test_totp_cfg();
	test_copy_forms();
	test_show_run_filters();
	test_show_if_detail();
	test_ping_cli();
	test_log();
	test_lldp();
	test_clear_counters();
	test_errdisable();
	test_ios_compat();
	printf("\n%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
