/*
 * env_cli.c - what cli.c links against besides support.c: the hostname,
 * the legacy-parser fallback and the bridged actions, all recorded so
 * tests can assert on them.
 */
#include <stdint.h>
#include <stdio.h>

#include "rtl837x_common.h"
#include "support.h"

char hostname[24] = "sw";

char last_fallback[CMD_BUF_SIZE];
int n_fallback, n_save, n_reset, n_showver;

void env_cli_reset(void)
{
	last_fallback[0] = 0;
	n_fallback = n_save = n_reset = n_showver = 0;
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
