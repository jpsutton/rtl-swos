/*
 * test_replay.c - replay a config file through the boot-time replay path
 * of the real CLI and fail on any error line. Used on the output of
 * tools/convert-legacy-config.py, so a converted config is known to boot
 * cleanly before it goes near a switch.
 *
 *   build/test_replay file.cfg [expected-substring...]
 *
 * Every further argument must appear in the running config rendered
 * after the replay.
 */
#include "sdcc_shim.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "cli.h"
#include "swcfg.h"
#include "runcfg.h"
#include "hw_mock.h"
#include "support.h"

void env_cli_reset(void);
void stp_test_defaults(void);
void fake_flash_reset(void);
void vlan_setup(void);

int main(int argc, char **argv)
{
	static char text[CONFIG_LEN * 2], line[CMD_BUF_SIZE];
	FILE *f;
	size_t n;
	int i, len = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: %s file.cfg [expected...]\n", argv[0]);
		return 2;
	}
	f = fopen(argv[1], "r");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	n = fread(text, 1, sizeof(text) - 1, f);
	fclose(f);
	text[n] = 0;

	hw_reset();
	for (int lp = 0; lp < 9; lp++)
		hw_reg_set(RTL8373_REG_MAC_L2_PORT_MAX_LEN + (lp << 8), 0x3fff);
	fake_flash_reset();
	env_cli_reset();
	stp_test_defaults();
	vlan_setup();
	sw_init();
	cli_init();

	printf("== replaying %s ==\n", argv[1]);
	out_reset();
	cli_replay_begin();
	for (char *t = text;; t++) {
		if (*t == '\n' || *t == 0) {
			line[len] = 0;
			if (len)
				cli_replay_line(line);
			len = 0;
			if (!*t)
				break;
			continue;
		}
		if (*t != '\r' && len < CMD_BUF_SIZE - 1)
			line[len++] = *t;
	}
	cli_replay_end();

	CHECK(!strstr(out_buf, "% "), "no errors during the replay");
	if (strstr(out_buf, "% "))
		printf("--- replay output ---\n%s", out_buf);

	n = runcfg_render();
	CHECK(n != 0xffff, "renders into the config sector");
	for (i = 2; i < argc; i++) {
		CHECK(strstr((char *)cfg_buf, argv[i]) != NULL, argv[i]);
	}
	printf("%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
