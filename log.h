#ifndef _LOG_H_
#define _LOG_H_

#include <stdint.h>

/*
 * The local event log: `show logging` shows it, `clear logging` empties it.
 * Each entry is one line, "Sep 22 22:23:22 %TAG: text" once NTP has set
 * the clock, "*00:12:34 %TAG: text" (uptime) before. Entries are also
 * printed on the console, which forwards to the syslog server.
 *
 * These functions live in HOME, not in a bank: callers anywhere pass their
 * own string constants, which must be read with the caller's bank mapped.
 * An entry is log_begin(), any number of the others, log_end().
 */
#define LOG_SIZE 2048

extern __xdata char log_buf[LOG_SIZE];
extern __xdata uint16_t log_head;	/* next write position */
extern __xdata uint8_t log_wrapped;	/* the buffer has been filled once */

void log_begin(__code const char *tag);
void log_s(__code const char *s);
void log_dec(uint16_t v);
void log_ip(__xdata const uint8_t *a);
void log_if(uint8_t lport);		/* "Ethernet1/N" */
void log_end(void);
void log_clear(void);

/* Link supervision: logs ports that went up or down since the last call */
void log_links(void) __banked;

#endif
