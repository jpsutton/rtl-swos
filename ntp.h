#ifndef _NTP_H_
#define _NTP_H_

#include <stdint.h>
#include "dns.h"

#define NTP_DST_OFF	0
#define NTP_DST_EU	1
#define NTP_DST_US	2

struct uip_udp_conn;

struct ntp_state {
	uint8_t enabled;
	char server[DNS_NAME_LEN];	/* host name or dotted address */
	uint8_t addr[4];	/* address of the server last used */
	uint16_t interval;	/* minutes between two synchronisations */
	int16_t offset;		/* time zone, minutes east of UTC */
	uint8_t dst;		/* NTP_DST_* */
	char tz_name[8];	/* clock timezone NAME */
	char dst_name[8];	/* clock summer-time NAME */
	uint8_t phase;
	uint8_t tries;
	uint8_t synced;
	uint8_t stratum;
	uint32_t utc;		/* seconds since 1970 UTC at the last synchronisation */
	uint32_t at;		/* uptime seconds at the last synchronisation */
	uint32_t next;		/* uptime seconds of the next synchronisation */
	uint32_t sent;		/* ticks when the request went out */
	struct uip_udp_conn *conn;
};

extern __xdata struct ntp_state ntp_state;

void ntp_init(void) __banked;
uint32_t ntp_unix_now(void) __banked __reentrant;
/* local time into ntp_year, ntp_mon, ntp_mday, ntp_hour, ntp_min, ntp_sec;
 * 0 while not synchronised, 2 in summer time */
uint8_t ntp_local_now(void) __banked __reentrant;
extern __xdata uint16_t ntp_year;
extern __xdata uint8_t ntp_mon, ntp_mday, ntp_hour, ntp_min, ntp_sec;
void ntp_start(void) __banked;
void ntp_stop(void) __banked;
void ntp_show(void) __banked __reentrant;		/* show ntp */
void ntp_show_time(void) __banked __reentrant;	/* show clock */
void ntp_callback(uint16_t lport) __banked __reentrant;

#endif
