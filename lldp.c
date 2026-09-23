/*
 * LLDP for the RTL837x platform. See lldp.h.
 *
 * Locals live in xdata and functions take at most one register parameter:
 * the 8051's internal RAM has nothing left.
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "machine.h"
#include "uip.h"
#include "swcfg.h"
#include "console.h"
#include "version.h"
#include "lldp.h"

#pragma codeseg BANK4
#pragma constseg BANK4

extern __code const struct machine machine;
extern __xdata struct machine_runtime machine_detected;
extern __xdata uint8_t uip_buf[];
extern __xdata char hostname[24];

__xdata uint8_t lldp_enabled;
__xdata uint16_t lldp_no_tx, lldp_no_rx;
__xdata struct lldp_nb lldp_nb[LLDP_PORTS];

static __xdata uint8_t tx_in[LLDP_PORTS];	/* seconds until the next LLDPDU */
static __xdata uint16_t links;
static __xdata uint8_t lp_, i_;
static __xdata uint16_t len_, pos_;
static __xdata uint8_t * __xdata pp;

/* Received: CPU tag and 802.1Q tag precede the EtherType. Sent: the TX
 * descriptor comes first, no 802.1Q tag. */
#define RX_PORT		19
#define RX_ETYPE	24
#define RX_TLV		26
#define TX_BASE		RTL_FRAME_DESC_SIZE
#define TX_TLV		22

#define CAP_BRIDGE	0x0004

static __code const char hexd[] = "0123456789abcdef";


void lldp_init(void) __banked
{
	lldp_enabled = 0;
	lldp_no_tx = lldp_no_rx = 0;
	for (lp_ = 0; lp_ < LLDP_PORTS; lp_++) {
		lldp_nb[lp_].ttl = 0;
		tx_in[lp_] = 1;
	}
	links = 0;
}


void lldp_fdb_refresh(void) __banked
{
	for (i_ = 0; i_ < SW_MAX_VLANS; i_++)
		if (sw_vlans[i_])
			port_l2mc_set(0x0e, sw_vlans[i_], lldp_enabled ? PMASK_CPU
				      : PMASK_CPU | (machine_detected.isRTL8373 ? PMASK_9 : PMASK_6));
}


void lldp_enable(__xdata uint8_t on) __banked
{
	lldp_enabled = on;
	for (lp_ = 0; lp_ < LLDP_PORTS; lp_++) {
		lldp_nb[lp_].ttl = 0;
		tx_in[lp_] = 1;
	}
	lldp_fdb_refresh();
}


/* ---- sending ---- */

static void put_tlv(uint8_t type)	/* header of a TLV of length len_ */
{
	pp[pos_++] = (type << 1) | (len_ >> 8);
	pp[pos_++] = len_;
}

static void put_code(__code const char *s)
{
	static __code const char * __xdata p;

	p = s;
	while (*p)
		pp[pos_++] = *p++;
}

static void put_x(__xdata const char *s)
{
	static __xdata const char * __xdata p;

	p = s;
	while (*p)
		pp[pos_++] = *p++;
}

static __xdata char ifname[12];
static void make_ifname(void)	/* "Ethernet1/N" for lp_ */
{
	static __xdata uint8_t up;

	for (up = 1; up <= 9; up++)
		if (machine.phys_to_log_port[up - 1] == lp_)
			break;
	for (i_ = 0; i_ < 10; i_++)
		ifname[i_] = "Ethernet1/"[i_];
	ifname[10] = '0' + up;
	ifname[11] = 0;
}

static uint8_t xlen(__xdata const char *s)
{
	static __xdata const char * __xdata p;
	static __xdata uint8_t n;

	p = s;
	for (n = 0; p[n]; n++)
		;
	return n;
}

static uint8_t clen(__code const char *s)
{
	static __code const char * __xdata p;
	static __xdata uint8_t n;

	p = s;
	for (n = 0; p[n]; n++)
		;
	return n;
}

static void lldp_tx(void)	/* out of port lp_ */
{
	pp = &uip_buf[TX_BASE];
	pp[0] = 0x01; pp[1] = 0x80; pp[2] = 0xc2; pp[3] = 0x00; pp[4] = 0x00; pp[5] = 0x0e;
	for (i_ = 0; i_ < 6; i_++)
		pp[6 + i_] = uip_ethaddr.addr[i_];
	pp[12] = RTL_FRAME_TAG_ID >> 8;
	pp[13] = RTL_FRAME_TAG_ID & 0xff;
	pp[14] = RTL_FRAME_TAG_VERSION;
	pp[15] = 0;
	pp[16] = 0;
	pp[17] = RTL_TAG_LEARN_DIS;
	len_ = (uint16_t)1 << lp_;
	pp[18] = len_ >> 8;
	pp[19] = len_;
	pp[20] = 0x88; pp[21] = 0xcc;
	pos_ = TX_TLV;
	make_ifname();
	/* chassis ID: MAC address */
	len_ = 7; put_tlv(1);
	pp[pos_++] = 4;
	for (i_ = 0; i_ < 6; i_++)
		pp[pos_++] = uip_ethaddr.addr[i_];
	/* port ID: interface name */
	len_ = 1 + 11; put_tlv(2);
	pp[pos_++] = 5;
	put_x(ifname);
	/* TTL */
	len_ = 2; put_tlv(3);
	pp[pos_++] = 0;
	pp[pos_++] = LLDP_HOLD;
	/* port description: the configured one, else the name */
	if (port_names[lp_][0]) {
		len_ = xlen(port_names[lp_]); put_tlv(4);
		put_x(port_names[lp_]);
	} else {
		len_ = 11; put_tlv(4);
		put_x(ifname);
	}
	/* system name, system description */
	len_ = xlen(hostname); put_tlv(5);
	put_x(hostname);
	len_ = clen("rtl-swos " VERSION_SW " ") + clen(machine.machine_name); put_tlv(6);
	put_code("rtl-swos " VERSION_SW " ");
	put_code(machine.machine_name);
	/* capabilities: bridge, enabled */
	len_ = 4; put_tlv(7);
	pp[pos_++] = 0; pp[pos_++] = CAP_BRIDGE;
	pp[pos_++] = 0; pp[pos_++] = CAP_BRIDGE;
	/* management address: IPv4, no interface number */
	len_ = 12; put_tlv(8);
	pp[pos_++] = 5;
	pp[pos_++] = 1;
	for (i_ = 0; i_ < 4; i_++)
		pp[pos_++] = ((__xdata uint8_t *)uip_hostaddr)[i_];
	pp[pos_++] = 1;
	pp[pos_++] = 0; pp[pos_++] = 0; pp[pos_++] = 0; pp[pos_++] = 0;
	pp[pos_++] = 0;
	/* end */
	pp[pos_++] = 0;
	pp[pos_++] = 0;
	while (pos_ < 60)
		pp[pos_++] = 0;
	uip_len = pos_;
	tcpip_output();
	uip_len = 0;
}


void lldp_link_up(void) __banked
{
	for (lp_ = 0; lp_ < LLDP_PORTS; lp_++)
		tx_in[lp_] = 1;		/* the next tick announces on every up port */
}


void lldp_tick(void) __banked
{
	static __xdata uint8_t lc;

	if (!lldp_enabled)
		return;
	for (lp_ = machine.min_port; lp_ <= machine.max_port; lp_++) {
		if (lldp_nb[lp_].ttl)
			lldp_nb[lp_].ttl--;
		lc = port_link_code(lp_);
		if (lc == PORT_LINK_DOWN) {
			lldp_nb[lp_].ttl = 0;
			continue;
		}
		if (tx_in[lp_] && --tx_in[lp_])
			continue;
		tx_in[lp_] = LLDP_TX_INTERVAL;
		if (!(lldp_no_tx & ((uint16_t)1 << lp_)))
			lldp_tx();
	}
}


/* ---- receiving ---- */

static __xdata uint8_t * __xdata dst;
static __xdata uint8_t t_type;
static __xdata uint16_t t_len;

/* Copy t_len bytes from pp[pos_] as text into dst, printable only */
static void get_txt(void)
{
	static __xdata uint8_t n, c;

	for (n = 0; n < t_len && n < LLDP_TXT - 1; n++) {
		c = pp[pos_ + n];
		dst[n] = (c >= 0x20 && c < 0x7f) ? c : '.';
	}
	dst[n] = 0;
}

/* A MAC address (6 bytes at pp[pos_]) as aabb.ccdd.eeff */
static void get_mac(void)
{
	static __xdata uint8_t n, k, c;

	k = 0;
	for (n = 0; n < 6; n++) {
		c = pp[pos_ + n];
		dst[k++] = hexd[c >> 4];
		dst[k++] = hexd[c & 15];
		if (n == 1 || n == 3)
			dst[k++] = '.';
	}
	dst[k] = 0;
}

void lldp_in(void) __banked
{
	static __xdata struct lldp_nb * __xdata nb;
	static __xdata uint8_t p;

	p = uip_buf[RX_PORT] & 0x0f;
	len_ = uip_len;
	uip_len = 0;
	if (!lldp_enabled || p < machine.min_port || p > machine.max_port
	    || (lldp_no_rx & ((uint16_t)1 << p))
	    || uip_buf[RX_ETYPE] != 0x88 || uip_buf[RX_ETYPE + 1] != 0xcc)
		return;
	nb = &lldp_nb[p];
	nb->chassis[0] = nb->port[0] = nb->sysname[0] = nb->portdesc[0] = nb->sysdesc[0] = 0;
	nb->mgmt[0] = nb->mgmt[1] = nb->mgmt[2] = nb->mgmt[3] = 0;
	nb->caps = 0;
	nb->ttl = 0;
	pp = uip_buf;
	pos_ = RX_TLV;
	while (pos_ + 2 <= len_) {
		t_type = pp[pos_] >> 1;
		t_len = ((uint16_t)(pp[pos_] & 1) << 8) | pp[pos_ + 1];
		pos_ += 2;
		if (!t_type || pos_ + t_len > len_)
			break;
		switch (t_type) {
		case 1:		/* chassis ID */
		case 2:		/* port ID */
			dst = t_type == 1 ? (__xdata uint8_t *)nb->chassis : (__xdata uint8_t *)nb->port;
			pos_++;
			t_len--;
			if (pp[pos_ - 1] == (t_type == 1 ? 4 : 3) && t_len == 6)
				get_mac();
			else
				get_txt();
			pos_--;
			t_len++;
			break;
		case 3:
			nb->ttl = ((uint16_t)pp[pos_] << 8) | pp[pos_ + 1];
			break;
		case 4:
			dst = (__xdata uint8_t *)nb->portdesc;
			get_txt();
			break;
		case 5:
			dst = (__xdata uint8_t *)nb->sysname;
			get_txt();
			break;
		case 6:
			dst = (__xdata uint8_t *)nb->sysdesc;
			get_txt();
			break;
		case 7:
			if (t_len >= 4)
				nb->caps = ((uint16_t)pp[pos_ + 2] << 8) | pp[pos_ + 3];
			break;
		case 8:		/* the first IPv4 management address */
			if (t_len >= 6 && pp[pos_] == 5 && pp[pos_ + 1] == 1 && !nb->mgmt[0])
				for (i_ = 0; i_ < 4; i_++)
					nb->mgmt[i_] = pp[pos_ + 2 + i_];
			break;
		}
		pos_ += t_len;
	}
}


/* ---- show lldp neighbors ---- */

static __xdata uint8_t col;

static void o_c(char c)
{
	write_char(c);
	col++;
}

static void o_x(__xdata const char *s)
{
	static __xdata const char * __xdata p;

	p = s;
	while (*p)
		o_c(*p++);
}

static void o_to(uint8_t c)
{
	static __xdata uint8_t to;

	to = c;
	do
		o_c(' ');
	while (col < to);
}

static __xdata uint16_t ov;
static void o_dec(void)
{
	static __xdata uint16_t d;
	static __xdata uint8_t lead;

	lead = 0;
	for (d = 10000; d; d /= 10) {
		if (ov >= d || lead || d == 1) {
			o_c('0' + ov / d);
			ov %= d;
			lead = 1;
		}
	}
}

static void o_field(__xdata const char *s)	/* text, or "-" when empty */
{
	static __xdata const char * __xdata p;

	p = s;
	if (*p)
		print_string_x((__xdata char *)p);
	else
		write_char('-');
}

/* Capabilities: B bridge, R router, W WLAN AP, T telephone, S station, O other */
static void o_caps(void)
{
	static __xdata uint8_t any;

	any = 0;
	if (ov & 0x0004) { o_c('B'); any = 1; }
	if (ov & 0x0010) { if (any) o_c(','); o_c('R'); any = 1; }
	if (ov & 0x0008) { if (any) o_c(','); o_c('W'); any = 1; }
	if (ov & 0x0020) { if (any) o_c(','); o_c('T'); any = 1; }
	if (ov & 0x0080) { if (any) o_c(','); o_c('S'); any = 1; }
	if (ov & 0x0001) { if (any) o_c(','); o_c('O'); any = 1; }
	if (!any)
		o_c('-');
}

void lldp_show(__xdata uint8_t detail) __banked
{
	static __xdata uint8_t n, dt;
	static __xdata struct lldp_nb * __xdata nb;

	dt = detail;
	if (!lldp_enabled) {
		print_string("% LLDP is not enabled (feature lldp)\n");
		return;
	}
	if (!dt)
		print_string("Capability codes: B - Bridge, R - Router, W - WLAN AP, T - Telephone,\n"
			     "                  S - Station, O - Other\n\n"
			     "Device ID               Local Intf  Hold-time  Capability  Port ID\n");
	n = 0;
	for (lp_ = machine.min_port; lp_ <= machine.max_port; lp_++) {
		nb = &lldp_nb[lp_];
		if (!nb->ttl)
			continue;
		n++;
		make_ifname();
		col = 0;
		if (!dt) {
			o_x(nb->sysname[0] ? nb->sysname : nb->chassis);
			o_to(24);
			o_c('E'); o_c('t'); o_c('h');
			o_x(ifname + 8);	/* "1/N" of "Ethernet1/N" */
			o_to(36);
			ov = nb->ttl;
			o_dec();
			o_to(47);
			ov = nb->caps;
			o_caps();
			o_to(59);
			o_x(nb->port);
			write_char('\n');
			continue;
		}
		print_string("Local interface: ");
		print_string_x(ifname);
		print_string("\n  Chassis ID: ");
		print_string_x(nb->chassis);
		print_string("\n  Port ID: ");
		print_string_x(nb->port);
		print_string("\n  Port description: ");
		o_field(nb->portdesc);
		print_string("\n  System name: ");
		o_field(nb->sysname);
		print_string("\n  System description: ");
		o_field(nb->sysdesc);
		print_string("\n  Capabilities: ");
		col = 0;
		ov = nb->caps;
		o_caps();
		print_string("\n  Management address: ");
		if (nb->mgmt[0] | nb->mgmt[1] | nb->mgmt[2] | nb->mgmt[3])
			print_ip(nb->mgmt);
		else
			write_char('-');
		print_string("\n  Hold time: ");
		itoa_short(nb->ttl);
		print_string(" s\n\n");
	}
	print_string("Total entries displayed: ");
	itoa_short(n);
	write_char('\n');
}
