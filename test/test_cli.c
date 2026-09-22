/*
 * test_cli.c - host unit tests for cli.c (the modal CLI engine).
 *
 * Drives cli_exec_line/cli_help/cli_complete directly with fabricated
 * lines and asserts on mode transitions, abbreviation, ambiguity and
 * error output, the legacy fallback, and the NX-OS-style exec-anywhere
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
extern char last_fallback[];
extern int n_fallback, n_save, n_reset, n_showver, n_setspeed;
extern uint8_t last_speed, last_port;
extern char port_names[9][PORT_NAME_SIZE];
void env_cli_reset(void);

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

static void reset_all(void)
{
	hw_reset();
	env_cli_reset();
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
	CHECK(n_showver == 1, "'sh ver' runs show version");
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
	CHECK(n_reset == 0 && n_fallback == 1,
	      "reload hidden in user EXEC falls through to legacy");
	run("enable");
	run("reload");
	CHECK(n_reset == 1, "reload works in privileged EXEC");
}

static void test_fallback(void)
{
	printf("[test] legacy fallback\n");
	reset_all();
	run("stat");
	CHECK(n_fallback == 1 && strcmp(last_fallback, "stat") == 0,
	      "unknown first word goes to the legacy parser verbatim");
	run("enable");
	run("port 5 1g");
	CHECK(n_fallback == 2 && strcmp(last_fallback, "port 5 1g") == 0,
	      "legacy config command falls through");
}

static void test_exec_anywhere(void)
{
	printf("[test] exec commands from config modes (no 'do')\n");
	reset_all();
	run("enable");
	run("configure terminal");
	run("show version");
	CHECK(n_showver == 1, "show version works in config mode");
	CHECK(cli.mode == CLI_MODE_CONFIG, "mode unchanged");
	run("write memory");
	CHECK(n_save == 1, "write memory works in config mode");
}

static void test_write_and_copy(void)
{
	printf("[test] write memory / copy running-config startup-config\n");
	reset_all();
	run("enable");
	run("write");
	CHECK(n_save == 1, "bare 'write' saves");
	run("copy running-config startup-config");
	CHECK(n_save == 2, "copy run start saves");
	run("copy tftp flash 10.0.0.1 fw.bin");
	CHECK(n_fallback == 1 &&
	      strcmp(last_fallback, "copy tftp flash 10.0.0.1 fw.bin") == 0,
	      "copy tftp passes the whole line to the legacy parser");
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
	CHECK(last_speed == PHY_SPEED_100M, "abbreviated 'sp 100' -> 100M");

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
	CHECK(n_fallback == 1 && !sw_vlan_exists(5), "a lone 'n' is not taken for 'no'");
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

int main(void)
{
	printf("== cli.c modal engine tests ==\n");
	test_modes();
	test_abbreviation();
	test_errors();
	test_priv_gating();
	test_fallback();
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
	printf("\n%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
