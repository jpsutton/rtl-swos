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

extern char last_fallback[];
extern int n_fallback, n_save, n_reset, n_showver;
void env_cli_reset(void);

static char linebuf[CMD_BUF_SIZE];

static void run(const char *s)
{
	strncpy(linebuf, s, sizeof(linebuf) - 1);
	linebuf[sizeof(linebuf) - 1] = 0;
	out_reset();
	cli_exec_line(linebuf);
}

static void reset_all(void)
{
	env_cli_reset();
	cli_init();
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
	run("no vlan 5");
	CHECK(out_has("'^' marker"), "no on a command without NO_OK errors");
	CHECK(cli.mode == CLI_MODE_CONFIG, "mode unchanged");
	run("no");
	CHECK(out_has("% Incomplete command"), "bare 'no' is incomplete");
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
	printf("\n%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
