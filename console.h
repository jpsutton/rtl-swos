#ifndef _CONSOLE_H_
#define _CONSOLE_H_

#include <stdint.h>

#include "rtl837x_common.h"

extern __xdata uint8_t cmd_buffer[CMD_BUF_SIZE];
extern __xdata uint8_t cmd_available;

void execute_config(void) __banked;
/* copy startup-config running-config: replay the startup config in the current session */
void config_merge(void) __banked;
void print_ip(__xdata uint8_t *ptr) __banked;
void clear_command_history(void) __banked;
void cmd_history_add(__xdata const char * __xdata line) __banked;

#endif
