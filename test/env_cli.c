/*
 * env_cli.c - what cli.c links against besides support.c: the hostname,
 * the legacy-parser fallback and the bridged actions, all recorded so
 * tests can assert on them.
 */
#include <stdint.h>
#include <stdio.h>

#include "rtl837x_common.h"
#include "rtl837x_phy.h"
#include "support.h"

char hostname[24] = "sw";

char last_fallback[CMD_BUF_SIZE];
int n_fallback, n_save, n_reset, n_showver;

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
