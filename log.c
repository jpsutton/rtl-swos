/*
 * Local event log. See log.h. The entry API is in HOME: this file has no
 * codeseg pragma (SDCC applies one to the whole file). log_links() is in
 * log_links.c, in BANK4.
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "machine.h"
#include "ntp.h"
#include "log.h"

extern __code const struct machine machine;
extern __xdata uint8_t sfr_data[4];

__xdata char log_buf[LOG_SIZE];
__xdata uint16_t log_head;
__xdata uint8_t log_wrapped;

static __code const char log_mon[] = "JanFebMarAprMayJunJulAugSepOctNovDec";


static void log_c(char c)
{
	log_buf[log_head++] = c;
	if (log_head == LOG_SIZE) {
		log_head = 0;
		log_wrapped = 1;
	}
	write_char(c);
}


static __xdata uint8_t l2;
static void log_2(uint8_t v)
{
	l2 = v;
	log_c('0' + l2 / 10);
	log_c('0' + l2 % 10);
}


void log_clear(void)
{
	log_head = 0;
	log_wrapped = 0;
}


/* Parameters are copied into xdata first: kept in the registers or
 * internal RAM across the calls below they would need internal RAM, which
 * has nothing left. */
void log_s(__code const char *s)
{
	static __code const char * __xdata p;

	p = s;
	while (*p)
		log_c(*p++);
}


void log_x(__xdata const char *s)
{
	static __xdata const char * __xdata p;

	p = s;
	while (*p)
		log_c(*p++);
}


void log_dec(uint16_t v)
{
	static __xdata uint16_t d, n;
	static __xdata uint8_t lead;

	n = v;
	lead = 0;
	for (d = 10000; d; d /= 10) {
		if (n >= d || lead || d == 1) {
			log_c('0' + n / d);
			n %= d;
			lead = 1;
		}
	}
}


void log_ip(__xdata const uint8_t *a)
{
	static __xdata const uint8_t * __xdata p;
	static __xdata uint8_t k;

	p = a;
	for (k = 0; k < 4; k++) {
		if (k)
			log_c('.');
		log_dec(p[k]);
	}
}


void log_if(uint8_t lport)
{
	static __xdata uint8_t up, lp;

	lp = lport;
	for (up = 1; up <= 9; up++)
		if (machine.phys_to_log_port[up - 1] == lp)
			break;
	log_s("Ethernet1/");
	log_dec(up);
}


void log_begin(__code const char *tag)
{
	static __xdata uint32_t t;
	static __code const char * __xdata tg;

	tg = tag;
	if (ntp_local_now()) {
		log_c(log_mon[3 * (ntp_mon - 1)]);
		log_c(log_mon[3 * (ntp_mon - 1) + 1]);
		log_c(log_mon[3 * (ntp_mon - 1) + 2]);
		log_c(' ');
		log_2(ntp_mday);
		log_c(' ');
		log_2(ntp_hour);
		log_c(':');
		log_2(ntp_min);
		log_c(':');
		log_2(ntp_sec);
	} else {
		reg_read_m(RTL837X_REG_SEC_COUNTER);
		t = ((uint32_t)sfr_data[0] << 24) | ((uint32_t)sfr_data[1] << 16)
		    | ((uint16_t)sfr_data[2] << 8) | sfr_data[3];
		log_c('*');
		log_2((uint8_t)((t / 3600) % 100));
		log_c(':');
		log_2((uint8_t)((t / 60) % 60));
		log_c(':');
		log_2((uint8_t)(t % 60));
	}
	log_s(" %");
	log_s(tg);
	log_s(": ");
}


void log_end(void)
{
	log_c('\n');
}
