/*
 * ping for the management interface. See ping.h.
 *
 * Echo requests are built directly in uip_buf (uIP itself only answers
 * pings), addressed by uip_arp_out() like any IP packet: a first request to
 * a host that is not in the ARP table becomes an ARP request instead and
 * times out, as in IOS. Round-trip times have the 5 ms resolution of the
 * system tick.
 */
#include <8051.h>
#include <stdint.h>
#include "rtl837x_common.h"
#include "console.h"
#include "uip.h"
#include "uip_arp.h"
#include "cli.h"
#include "dns.h"
#include "telnetd.h"
#include "ping.h"

#pragma codeseg BANK2
#pragma constseg BANK2

extern volatile __xdata uint32_t ticks;
extern __xdata uint8_t uip_buf[];

#define PING_ID		0x5357		/* "SW" */
#define PING_TIMEOUT	(2 * SYS_TICK_HZ)
#define MS_PER_TICK	(1000 / SYS_TICK_HZ)

/* offsets from the IP header, which starts at UIP_LLH_LEN */
#define IPH		(&uip_buf[UIP_LLH_LEN])
#define ICMPH		(&uip_buf[UIP_LLH_LEN + 20])

__xdata uint8_t ping_phase;
__xdata uint8_t ping_owner;

static __xdata uint8_t target[4];
static __xdata uint16_t count, size, sent, recv, seq;
static __xdata uint8_t got;		/* the reply to the current request arrived */
static __xdata uint16_t t_sent, rtt, rtt_min, rtt_max;
static __xdata uint32_t rtt_sum;
static __xdata uint16_t i_;
static __xdata uint32_t sum;


static __xdata uint16_t t_now;
static uint16_t now16(void)
{
	EA = 0;		/* ticks changes in the timer ISR */
	t_now = (uint16_t)ticks;
	EA = 1;
	return t_now;
}


/* Output goes to the session that started the run: over telnet it is
 * captured into the session's output buffer, which telnetd sends at its
 * next poll */
static void out_begin(void)
{
	if (ping_owner == CLI_VTY)
		telnet_capture = 1;
}

static void out_end(void)
{
	telnet_capture = 0;
}


static void print_dec(__xdata uint16_t v)
{
	itoa_short(v);
}


/* 16-bit one's complement sum over n bytes at p */
static __xdata uint8_t * __xdata cs_p;
static uint16_t cksum(__xdata uint16_t n)
{
	sum = 0;
	for (i_ = 0; i_ + 1 < n; i_ += 2)
		sum += ((uint16_t)cs_p[i_] << 8) | cs_p[i_ + 1];
	if (n & 1)
		sum += (uint16_t)cs_p[n - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);
	return ~(uint16_t)sum;
}


static void send_echo(void)
{
	static __xdata uint16_t c;

	cs_p = IPH;
	for (i_ = 0; i_ < size; i_++)
		cs_p[i_] = 0;
	/* ICMP echo request: id, sequence, a counting pattern as data */
	cs_p = ICMPH;
	cs_p[0] = 8;
	cs_p[4] = PING_ID >> 8;
	cs_p[5] = PING_ID & 0xff;
	cs_p[6] = seq >> 8;
	cs_p[7] = seq;
	for (i_ = 8; i_ < size - 20; i_++)
		cs_p[i_] = i_;
	c = cksum(size - 20);
	cs_p[2] = c >> 8;
	cs_p[3] = c;
	/* IPv4 header */
	cs_p = IPH;
	cs_p[0] = 0x45;
	cs_p[2] = size >> 8;
	cs_p[3] = size;
	cs_p[4] = seq >> 8;
	cs_p[5] = seq;
	cs_p[8] = 64;			/* TTL */
	cs_p[9] = 1;			/* ICMP */
	cs_p[12] = ((__xdata uint8_t *)uip_hostaddr)[0];
	cs_p[13] = ((__xdata uint8_t *)uip_hostaddr)[1];
	cs_p[14] = ((__xdata uint8_t *)uip_hostaddr)[2];
	cs_p[15] = ((__xdata uint8_t *)uip_hostaddr)[3];
	cs_p[16] = target[0];
	cs_p[17] = target[1];
	cs_p[18] = target[2];
	cs_p[19] = target[3];
	c = cksum(20);
	cs_p[10] = c >> 8;
	cs_p[11] = c;

	uip_len = size;		/* the IP datagram; uip_arp_out() adds the link-level header */
	uip_arp_out();		/* the ethernet header, or an ARP request instead */
	tcpip_output();
	uip_len = 0;
	sent++;
	got = 0;
	t_sent = now16();
}


static void finish(void)
{
	out_begin();
	print_string("\nSuccess rate is ");
	print_dec(sent ? (uint16_t)((uint32_t)recv * 100 / sent) : 0);
	print_string(" percent (");
	print_dec(recv);
	write_char('/');
	print_dec(sent);
	write_char(')');
	if (recv) {
		print_string(", round-trip min/avg/max = ");
		print_dec(rtt_min);
		write_char('/');
		print_dec((uint16_t)(rtt_sum / recv));
		write_char('/');
		print_dec(rtt_max);
		print_string(" ms");
	}
	write_char('\n');
	out_end();
	ping_phase = PING_IDLE;
	/* the prompt that was held back */
	if (ping_owner == CLI_VTY)
		telnet_prompt();
	else
		print_cmd_prompt();
}


static void header(void)
{
	print_string("Sending ");
	print_dec(count);
	print_string(", ");
	print_dec(size);
	print_string("-byte ICMP Echos to ");
	print_ip(target);
	print_string(", timeout is 2 seconds:\n");
}


void ping_start(__xdata char * __xdata host, __xdata uint16_t n, __xdata uint16_t len) __banked
{
	static __xdata uint8_t k;

	if (ping_phase) {
		print_string("% A ping is already running\n");
		return;
	}
	if (!(((__xdata uint8_t *)uip_hostaddr)[0] | ((__xdata uint8_t *)uip_hostaddr)[1]
	      | ((__xdata uint8_t *)uip_hostaddr)[2] | ((__xdata uint8_t *)uip_hostaddr)[3])) {
		print_string("% No management address\n");
		return;
	}
	for (k = 0; host[k] && host[k] != ' '; k++) {
		if (k == DNS_NAME_LEN - 1) {
			print_string("% Name too long\n");
			return;
		}
	}
	if (dns_state.status == DNS_PENDING) {
		print_string("% The resolver is busy, try again\n");
		return;
	}
	for (k = 0; host[k] && host[k] != ' '; k++)
		dns_state.name[k] = host[k];
	dns_state.name[k] = 0;
	count = n;
	size = len;
	sent = recv = 0;
	rtt_min = 0xffff;
	rtt_max = 0;
	rtt_sum = 0;
	ping_owner = cli_session();
	print_string("Type any key to abort.\n");
	dns_state.verbose = 0;
	dns_lookup();		/* a dotted address resolves at once */
	if (dns_state.status == DNS_DONE) {
		for (k = 0; k < 4; k++)
			target[k] = dns_state.addr[k];
		header();
		ping_phase = PING_SEND;
	} else if (dns_state.status == DNS_PENDING) {
		print_string("Translating \"");
		print_string_x(dns_state.name);
		print_string("\"...\n");
		ping_phase = PING_RESOLVE;
	} else {
		print_string("% Unrecognized host or address\n");
	}
}


void ping_abort(void) __banked
{
	if (ping_phase)
		finish();
}


void ping_reply(void) __banked
{
	static __xdata uint8_t k;

	if (ping_phase != PING_WAIT || got)
		return;
	cs_p = ICMPH;
	if (cs_p[4] != (PING_ID >> 8) || cs_p[5] != (PING_ID & 0xff)
	    || cs_p[6] != (uint8_t)(seq >> 8) || cs_p[7] != (uint8_t)seq)
		return;
	cs_p = IPH;
	for (k = 0; k < 4; k++)
		if (cs_p[12 + k] != target[k])
			return;
	got = 1;
	rtt = (now16() - t_sent) * MS_PER_TICK;
}


void ping_tick(void) __banked
{
	static __xdata uint8_t k;

	if (ping_owner == CLI_VTY && !telnet_state.conn) {	/* the session is gone */
		ping_phase = PING_IDLE;
		return;
	}
	switch (ping_phase) {
	case PING_RESOLVE:
		if (dns_state.status == DNS_PENDING)
			return;
		out_begin();
		if (dns_state.status != DNS_DONE) {
			print_string("% Unrecognized host or address\n");
			out_end();
			ping_phase = PING_IDLE;
			if (ping_owner == CLI_VTY)
				telnet_prompt();
			else
				print_cmd_prompt();
			return;
		}
		for (k = 0; k < 4; k++)
			target[k] = dns_state.addr[k];
		header();
		out_end();
		ping_phase = PING_SEND;
		return;
	case PING_SEND:
		seq++;
		send_echo();
		ping_phase = PING_WAIT;
		return;
	case PING_WAIT:
		if (got) {
			recv++;
			if (rtt < rtt_min)
				rtt_min = rtt;
			if (rtt > rtt_max)
				rtt_max = rtt;
			rtt_sum += rtt;
			out_begin();
			write_char('!');
			out_end();
		} else if ((uint16_t)(now16() - t_sent) >= PING_TIMEOUT) {
			out_begin();
			write_char('.');
			out_end();
		} else {
			return;
		}
		if (sent < count)
			ping_phase = PING_SEND;
		else
			finish();
		return;
	}
}
