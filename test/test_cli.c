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
#include "runcfg.h"
#include "telnetd.h"
#include "dhcp.h"
#include "rtl837x_regs.h"
extern char last_fallback[];
extern int n_fallback, n_reset, n_showver, n_setspeed;
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

void fake_flash_reset(void);

static void reset_all(void)
{
	hw_reset();
	for (int lp = 0; lp < 9; lp++)	/* MTU registers power up at 16383 */
		hw_reg_set(RTL8373_REG_MAC_L2_PORT_MAX_LEN + (lp << 8), 0x3fff);
	fake_flash_reset();
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
	printf("[test] boot replay: legacy syntax shim, comments, deferred push\n");
	wipe_all();
	unsigned long w0;
	replay_text("! a comment\n"
		    "ip 192.168.10.247\n"		/* legacy: invalid new syntax */
		    "netmask 255.255.255.0\n"		/* legacy: unknown word */
		    "telnet on\n"
		    "   ! indented comment\n"
		    "vlan 30\n"				/* new syntax still works */
		    " name lab\n");
	CHECK(n_fallback == 3, "the three legacy lines went to the legacy parser");
	CHECK(strstr(last_fallback, "telnet on") != NULL, "verbatim");
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
	CHECK(out_has("'^' marker") && n_fallback == 0, "interactive lines are not shimmed");
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
	test_runcfg_defaults();
	test_runcfg_roundtrip();
	test_write_and_startup();
	test_replay_legacy_and_comments();
	test_physical_shadows();
	printf("\n%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
