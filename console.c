/*
 * Console plumbing: the serial line buffer, the recall history, and the
 * boot-time replay of the startup config through the CLI.
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_flash.h"
#include "cli.h"
#include "console.h"

#pragma codeseg BANK2
#pragma constseg BANK2

extern __xdata uint8_t flash_buf[FLASH_BUF_SIZE];
extern __xdata struct flash_region_t flash_region;
extern __xdata char passwd[21];

/* shared tables the drivers and the CLI fill */
__xdata uint8_t vlan_names[VLAN_NAMES_SIZE];
__xdata uint16_t vlan_ptr;
__xdata char port_names[9][PORT_NAME_SIZE];

__xdata uint8_t cmd_buffer[CMD_BUF_SIZE];
__xdata uint8_t cmd_available;
__xdata uint8_t cmd_history[CMD_HISTORY_SIZE];
__xdata uint16_t cmd_history_ptr;


void print_ip(__xdata uint8_t * ptr) __banked
{
	uint8_t idx = 0;
	uint8_t num;

	while(1) {
		num = *ptr++;
		itoa(num);
		if (++idx == 4)
			break;

		write_char('.');
	}
}


void clear_command_history(void) __banked
{
	for (cmd_history_ptr = 0; cmd_history_ptr < CMD_HISTORY_SIZE; cmd_history_ptr++)
		cmd_history[cmd_history_ptr] = 0;
	cmd_history_ptr = 0;
	return;
}


#define FLASH_READ_BURST_SIZE 0x100

#if CONFIG_LEN % FLASH_READ_BURST_SIZE
	#error "CONFIG_LEN not a multiple of FLASH_READ_BURST_SIZE"
#endif
void execute_config(void) __banked
{
	__xdata uint32_t pos = CONFIG_START;
	__xdata uint8_t pages_left = CONFIG_LEN / FLASH_READ_BURST_SIZE;
	__xdata uint8_t skipping = 0;

	// Set default password, it can be overwritten in the configuration file
	strtox(passwd, DEFAULT_PASSWORD);
	cli_replay_begin();

	uint8_t cmd_idx = 0;
	do {
		flash_region.addr = pos;
		flash_region.len = FLASH_READ_BURST_SIZE;
		flash_read_bulk(flash_buf);

		__xdata uint8_t cfg_idx = 0;
		uint8_t c = 0;
		do {
			c = flash_buf[cfg_idx++];
			if (c == '\r')
				continue;	/* configs uploaded over TFTP may be CRLF */
			/* NUL ends a saved config; 0xff is erased flash */
			if (c == 0 || c == 0xff || c == '\n') {
				cmd_buffer[cmd_idx] = NUL;
				if (cmd_idx && !skipping)
					cli_replay_line((__xdata char *)cmd_buffer);
				if (c != '\n')
					goto config_done;
				cmd_idx = 0;
				skipping = 0;
				continue;
			}
			if (skipping)
				continue;
			if (cmd_idx >= (CMD_BUF_SIZE - 1)) {
				cmd_buffer[cmd_idx] = NUL;
				print_string("% Config line too long, skipped: ");
				print_string_x(cmd_buffer);
				write_char('\n');
				skipping = 1;
				continue;
			}
			cmd_buffer[cmd_idx] = c;
			cmd_idx++;
		} while (cfg_idx);

		pages_left--;
		pos += FLASH_READ_BURST_SIZE;
	} while(pages_left);

config_done:
	cli_replay_end();
	clear_command_history();
}


void cmd_history_add(__xdata const char * __xdata line) __banked
{
	static __xdata uint8_t n, i;
	static __xdata uint16_t p;

	for (n = 0; line[n] && n < CMD_BUF_SIZE - 1; n++)
		;
	if (!n)
		return;
	p = cmd_history_ptr;
	for (i = 0; i < n; i++) {
		cmd_history[p] = line[i];
		p = (p + 1) & CMD_HISTORY_MASK;
	}
	cmd_history[p] = '\n';
	cmd_history_ptr = (p + 1) & CMD_HISTORY_MASK;
}

