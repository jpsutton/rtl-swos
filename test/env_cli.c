/*
 * env_cli.c - the cli.c / swcfg.c edges that are not the port driver:
 * the hostname, the legacy-parser fallback, the bridged actions, the PHY,
 * IGMP, DHCP and syslog entry points, all recorded so tests can assert on
 * them. Linked by every test binary together with env_tables.c.
 */
#include <stdint.h>
#include <stdio.h>

#include "rtl837x_common.h"
#include "rtl837x_phy.h"
#include "uip.h"
#include "dhcp.h"
#include "syslog.h"
#include "support.h"

char hostname[24] = "sw";

char last_fallback[CMD_BUF_SIZE];
int n_fallback, n_save, n_reset, n_showver;
int n_igmp_on, n_igmp_off, n_hostdef, n_dhcp_start, n_dhcp_stop;
int n_syslog_start, n_syslog_stop;
struct dhcp_state dhcp_state;

/* phy driver edges the interface handlers touch */
struct phy_settings phy_settings;
char port_names[9][PORT_NAME_SIZE];
int n_setspeed;
uint8_t last_speed, last_port;

void phy_set_speed(void)
{
	n_setspeed++;
	last_speed = phy_settings.speed;
	last_port = phy_settings.port;
}

void env_cli_reset(void)
{
	last_fallback[0] = 0;
	n_fallback = n_save = n_reset = n_showver = n_setspeed = 0;
	n_igmp_on = n_igmp_off = n_hostdef = n_dhcp_start = n_dhcp_stop = 0;
	n_syslog_start = n_syslog_stop = 0;
	syslog_state.enabled = 0;
	dhcp_state.state = 0;
	last_speed = last_port = 0;
	for (int i = 0; i < 9; i++)
		port_names[i][0] = 0;
}

void itoa_short(uint16_t v)
{
	char b[6];
	int n = 0;

	do {
		b[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n)
		write_char(b[--n]);
}

void execute_commands(uint8_t *p)
{
	int i = 0;

	while (p[i] && i < CMD_BUF_SIZE - 1) {
		last_fallback[i] = p[i];
		i++;
	}
	last_fallback[i] = 0;
	n_fallback++;
}

void print_sw_version(void)
{
	print_string((char *)"VERSION\n");
	n_showver++;
}

void cmd_save_config(void)
{
	n_save++;
}

void reset_chip(void)
{
	n_reset++;
}

void igmp_enable(void) { n_igmp_on++; }
void igmp_setup(void) { n_igmp_off++; }

/* boot.c: derives a name from the MAC when none is configured */
void set_hostname_default(void)
{
	n_hostdef++;
	if (!hostname[0]) {
		const char *d = "default";
		int i = 0;
		while (d[i]) {
			hostname[i] = d[i];
			i++;
		}
		hostname[i] = 0;
	}
}

void dhcp_start(void) { n_dhcp_start++; dhcp_state.state = 1; }
void dhcp_stop(void) { n_dhcp_stop++; dhcp_state.state = 0; }

/* syslog_state itself lives in env_tables.c */
void syslog_start(void) { n_syslog_start++; syslog_state.enabled = 1; }
void syslog_stop(void) { n_syslog_stop++; syslog_state.enabled = 0; }
