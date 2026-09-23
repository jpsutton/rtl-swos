/*
 * LACP (IEEE 802.1AX) on top of the ASIC's static port-channels. See lacp.h.
 *
 * A reduced but interoperable implementation: the receive, periodic
 * transmission, selection and coupled mux machines of the standard,
 * collapsed into a 10 Hz tick and an evaluation per port-channel. No
 * marker protocol (receivers use it only to speed up a move; nobody
 * requires an answer), no churn detection.
 *
 * Locals live in xdata and functions take at most one register parameter:
 * the 8051's internal RAM has nothing left (see doc/xram.md).
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_sfr.h"
#include "rtl837x_regs.h"
#include "rtl837x_port.h"
#include "machine.h"
#include "uip.h"
#include "lacp.h"
#include "swcfg.h"
#include "log.h"

#pragma codeseg BANK2
#pragma constseg BANK2

extern __code const struct machine machine;
extern __xdata uint8_t uip_buf[];
extern __xdata uint8_t sfr_data[4];

__xdata uint8_t lacp_group[LACP_PORTS];
__xdata uint8_t lacp_mode[LACP_PORTS];
__xdata uint8_t lacp_fast[LACP_PORTS];
__xdata uint16_t lacp_pprio[LACP_PORTS];
__xdata uint16_t lacp_sysprio;
__xdata uint8_t lacp_minlinks[4];

__xdata uint8_t lacp_actor_st[LACP_PORTS];
__xdata uint8_t lacp_partner_st[LACP_PORTS];
__xdata uint16_t lacp_bundled;
__xdata uint16_t lacp_ports;

/* Partner information, as the partner announced itself */
static __xdata uint16_t p_sysprio[LACP_PORTS];
static __xdata uint8_t p_sys[LACP_PORTS][6];
static __xdata uint16_t p_key[LACP_PORTS];
static __xdata uint16_t p_pprio[LACP_PORTS];
static __xdata uint16_t p_port[LACP_PORTS];
/* The partner's view of us matches what we announce */
static __xdata uint8_t p_matched[LACP_PORTS];

static __xdata uint16_t cur_while[LACP_PORTS];	/* ticks until the partner info expires */
static __xdata uint16_t per_while[LACP_PORTS];	/* ticks until the next periodic LACPDU */
static __xdata uint8_t ntt[LACP_PORTS];		/* need to transmit */
static __xdata uint16_t rx_cnt[LACP_PORTS], tx_cnt[LACP_PORTS];
static __xdata uint8_t why[LACP_PORTS];		/* LW_*: why a port is (not) bundled */
static __xdata uint16_t links;			/* link-up ports, as of the last tick */
static __xdata uint16_t rx_ignored;		/* frames to the LACP address not taken */
static __xdata uint8_t rx_last[8];		/* start of the last one ignored, for show lacp */

#define LW_DOWN		0
#define LW_NOPARTNER	1	/* no (current) LACPDU from an aggregatable partner */
#define LW_OTHER	2	/* the partner is not the port-channel's partner */
#define LW_WAITING	3	/* selected; waiting for the partner to sync */
#define LW_MINLINKS	4	/* ready, but too few ports to meet min-links */
#define LW_BUNDLED	5

#define SHORT_TIMEOUT	(3 * LACP_TICK_HZ)
#define LONG_TIMEOUT	(90 * LACP_TICK_HZ)
#define FAST_PERIODIC	(1 * LACP_TICK_HZ)
#define SLOW_PERIODIC	(30 * LACP_TICK_HZ)

/* Offsets in a received frame: CPU tag and 802.1Q tag precede the
 * EtherType. In a frame we send, the TX descriptor comes first and there
 * is no 802.1Q tag. */
#define RX_PORT		19	/* low nibble: ingress port */
#define RX_ETYPE	24
#define RX_PDU		26
#define TX_BASE		RTL_FRAME_DESC_SIZE
#define TX_PDU		22
#define LACPDU_LEN	110
#define TX_LEN		(TX_PDU + LACPDU_LEN)

/* Offsets inside an LACPDU (subtype first) */
#define PDU_ACTOR	4	/* system priority of the actor TLV */
#define PDU_PARTNER	24	/* same in the partner TLV */
#define I_SYSPRIO	0
#define I_SYS		2
#define I_KEY		8
#define I_PPRIO		10
#define I_PORT		12
#define I_STATE		14

static __xdata uint8_t lp_, g_, i_;
static __xdata uint16_t bit_;
static __xdata uint8_t * __xdata pp;


void lacp_init(void) __banked
{
	for (lp_ = 0; lp_ < LACP_PORTS; lp_++) {
		lacp_group[lp_] = 0;
		lacp_mode[lp_] = 0;
		lacp_fast[lp_] = 0;
		lacp_pprio[lp_] = 32768;
		lacp_actor_st[lp_] = 0;
		lacp_partner_st[lp_] = 0;
		p_matched[lp_] = 0;
		cur_while[lp_] = 0;
		per_while[lp_] = 0;
		ntt[lp_] = 0;
		rx_cnt[lp_] = tx_cnt[lp_] = 0;
		why[lp_] = LW_DOWN;
	}
	for (g_ = 0; g_ < 4; g_++)
		lacp_minlinks[g_] = 1;
	lacp_sysprio = 32768;
	lacp_bundled = 0;
	lacp_ports = 0;
	links = 0;
}


static void partner_clear(uint8_t lp)
{
	static __xdata uint8_t p;

	p = lp;
	lacp_partner_st[p] = 0;
	p_matched[p] = 0;
	cur_while[p] = 0;
	p_sysprio[p] = 0;
	p_key[p] = 0;
	p_pprio[p] = 0;
	p_port[p] = 0;
	for (i_ = 0; i_ < 6; i_++)
		p_sys[p][i_] = 0;
}


/* Add a port to, or drop it from, its port-channel's member mask */
static __xdata uint8_t mask_on;
static void mask_set(uint8_t lp)
{
	static __xdata uint8_t p, g;
	static __xdata uint16_t b, m;

	p = lp;
	g = lacp_group[p] - 1;
	b = (uint16_t)1 << p;
	m = port_lag_members_get(g);
	if (mask_on) {
		lacp_bundled |= b;
		if (!(m & b)) {
			port_lag_members_set(g, m | b);
			log_begin("LACP-5-BUNDLE");
			log_if(p);
			log_s(" joined port-channel ");
			log_dec(g + 1);
			log_end();
		}
	} else {
		lacp_bundled &= ~b;
		if (m & b) {
			port_lag_members_set(g, m & ~b);
			log_begin("LACP-5-UNBUNDLE");
			log_if(p);
			log_s(" left port-channel ");
			log_dec(g + 1);
			log_end();
		}
	}
}


void lacp_port_set(uint8_t lport, __xdata uint8_t group, __xdata uint8_t mode) __banked
{
	static __xdata uint8_t p;

	p = lport;
	if (p >= LACP_PORTS)
		return;
	if (lacp_group[p] && (lacp_bundled & ((uint16_t)1 << p))) {
		mask_on = 0;
		mask_set(p);
	}
	partner_clear(p);
	lacp_actor_st[p] = 0;
	why[p] = LW_NOPARTNER;
	per_while[p] = 0;	/* an active port announces itself at once */
	ntt[p] = 0;
	if (!group) {
		lacp_group[p] = 0;
		lacp_mode[p] = 0;
		lacp_ports &= ~((uint16_t)1 << p);
		if (!lacp_ports)
			lacp_fdb_refresh();	/* back to discarding */
		return;
	}
	lacp_group[p] = group;
	lacp_mode[p] = mode;
	lacp_ports |= (uint16_t)1 << p;
	lacp_fdb_refresh();
}


void lacp_port_changed(uint8_t lport) __banked
{
	if (lport < LACP_PORTS && lacp_group[lport]) {
		ntt[lport] = 1;
		if (lacp_fast[lport] && cur_while[lport] > SHORT_TIMEOUT)
			cur_while[lport] = SHORT_TIMEOUT;
	}
}


/*
 * The ASIC discards the Slow Protocols address by default. While LACP runs
 * it forwards it instead, and a static entry in every VLAN a frame can
 * classify into (the VLAN database, which holds every PVID) confines it to
 * the CPU: a bridge must not pass LACPDUs on, whichever port they come in.
 */
void lacp_fdb_refresh(void) __banked
{
	if (!lacp_ports) {
		REG_SET(RTL837X_RMA_CTRL(2), RMA_ACT_DISCARD);
		return;
	}
	for (i_ = 0; i_ < SW_MAX_VLANS; i_++)
		if (sw_vlans[i_])
			port_l2mc_set(0x02, sw_vlans[i_], PMASK_CPU);
	REG_SET(RTL837X_RMA_CTRL(2), RMA_ACT_FORWARD);
}


/* ---- comparing and copying the fields of an LACPDU ---- */

static uint16_t get16(uint8_t off)
{
	return ((uint16_t)pp[off] << 8) | pp[off + 1];
}

static void put16(uint8_t off)
{
	pp[off] = bit_ >> 8;
	pp[off + 1] = bit_;
}


/* Does the partner info of port lp_ name the same system and key as that
 * of port i_? */
static uint8_t same_partner(void)
{
	static __xdata uint8_t k;

	if (p_sysprio[lp_] != p_sysprio[i_] || p_key[lp_] != p_key[i_])
		return 0;
	for (k = 0; k < 6; k++)
		if (p_sys[lp_][k] != p_sys[i_][k])
			return 0;
	return 1;
}


static uint8_t partner_valid(uint8_t lp)
{
	return cur_while[lp] && (lacp_partner_st[lp] & LACP_ST_AGGREGATION)
	       && (links & ((uint16_t)1 << lp));
}


/*
 * Selection and mux for port-channel g_+1. The port-channel's partner is
 * the one a bundled member already has, else the one of its lowest port
 * with a usable partner; ports facing another system stay individual.
 * A selected port reports sync; it is bundled once its partner reports
 * sync too, and min-links of them are ready.
 */
static void group_eval(void)
{
	static __xdata uint8_t ref, ready, st;
	static __xdata uint16_t readym;

	ref = 0xff;
	for (lp_ = 0; lp_ < LACP_PORTS && ref == 0xff; lp_++)
		if (lacp_group[lp_] == g_ + 1 && (lacp_bundled & ((uint16_t)1 << lp_)) && partner_valid(lp_))
			ref = lp_;
	for (lp_ = 0; lp_ < LACP_PORTS && ref == 0xff; lp_++)
		if (lacp_group[lp_] == g_ + 1 && partner_valid(lp_))
			ref = lp_;

	ready = 0;
	readym = 0;
	for (lp_ = 0; lp_ < LACP_PORTS; lp_++) {
		if (lacp_group[lp_] != g_ + 1)
			continue;
		bit_ = (uint16_t)1 << lp_;
		st = LACP_ST_AGGREGATION;
		if (lacp_mode[lp_] == LACP_MODE_ACTIVE)
			st |= LACP_ST_ACTIVITY;
		if (lacp_fast[lp_])
			st |= LACP_ST_TIMEOUT;
		if (!(links & bit_)) {
			why[lp_] = LW_DOWN;
			st |= LACP_ST_DEFAULTED;
		} else if (!partner_valid(lp_)) {
			why[lp_] = LW_NOPARTNER;
			st |= LACP_ST_DEFAULTED;
		} else {
			i_ = ref;
			if (!same_partner()) {
				why[lp_] = LW_OTHER;
			} else {
				st |= LACP_ST_SYNC;
				if (p_matched[lp_] && (lacp_partner_st[lp_] & LACP_ST_SYNC)) {
					ready++;
					readym |= bit_;
					why[lp_] = LW_MINLINKS;
				} else {
					why[lp_] = LW_WAITING;
				}
			}
		}
		lacp_actor_st[lp_] = st;	/* collecting/distributing below */
	}

	if (ready < lacp_minlinks[g_])
		readym = 0;
	for (lp_ = 0; lp_ < LACP_PORTS; lp_++) {
		if (lacp_group[lp_] != g_ + 1)
			continue;
		bit_ = (uint16_t)1 << lp_;
		st = lacp_actor_st[lp_];
		if (readym & bit_) {
			why[lp_] = LW_BUNDLED;
			st |= LACP_ST_COLLECTING | LACP_ST_DISTRIBUTING;
		}
		if (((lacp_bundled & bit_) != 0) != ((readym & bit_) != 0)) {
			mask_on = (readym & bit_) != 0;
			mask_set(lp_);
		}
		lacp_actor_st[lp_] = st;	/* eval_all() announces changes */
	}
}


static void eval_all(void)
{
	static __xdata uint8_t prev[LACP_PORTS];

	for (lp_ = 0; lp_ < LACP_PORTS; lp_++)
		prev[lp_] = lacp_actor_st[lp_];
	for (g_ = 0; g_ < 4; g_++)
		group_eval();
	for (lp_ = 0; lp_ < LACP_PORTS; lp_++)
		if (lacp_group[lp_] && prev[lp_] != lacp_actor_st[lp_])
			ntt[lp_] = 1;
}


/* Write the actor or partner info at TLV offset i_ of the PDU at pp */
static void tx_info(uint8_t lp)
{
	static __xdata uint8_t p, o;

	p = lp;
	o = i_;
	pp[o - 2] = (o == PDU_ACTOR) ? 1 : 2;	/* TLV type */
	pp[o - 1] = 20;				/* TLV length */
	if (o == PDU_ACTOR) {
		bit_ = lacp_sysprio; put16(o + I_SYSPRIO);
		for (g_ = 0; g_ < 6; g_++)
			pp[o + I_SYS + g_] = uip_ethaddr.addr[g_];
		bit_ = lacp_group[p]; put16(o + I_KEY);
		bit_ = lacp_pprio[p]; put16(o + I_PPRIO);
		bit_ = p + 1; put16(o + I_PORT);
		pp[o + I_STATE] = lacp_actor_st[p];
	} else {
		bit_ = p_sysprio[p]; put16(o + I_SYSPRIO);
		for (g_ = 0; g_ < 6; g_++)
			pp[o + I_SYS + g_] = p_sys[p][g_];
		bit_ = p_key[p]; put16(o + I_KEY);
		bit_ = p_pprio[p]; put16(o + I_PPRIO);
		bit_ = p_port[p]; put16(o + I_PORT);
		pp[o + I_STATE] = lacp_partner_st[p];
	}
}


static void lacp_tx(uint8_t lp)
{
	static __xdata uint8_t p;

	p = lp;
	pp = &uip_buf[TX_BASE];
	for (i_ = 0; i_ < TX_LEN; i_++)
		pp[i_] = 0;
	pp[0] = 0x01; pp[1] = 0x80; pp[2] = 0xc2; pp[5] = 0x02;
	for (i_ = 0; i_ < 6; i_++)
		pp[6 + i_] = uip_ethaddr.addr[i_];
	/* CPU tag: send out of this port only, do not learn */
	pp[12] = RTL_FRAME_TAG_ID >> 8;
	pp[13] = RTL_FRAME_TAG_ID & 0xff;
	pp[14] = RTL_FRAME_TAG_VERSION;
	pp[17] = RTL_TAG_LEARN_DIS;
	bit_ = (uint16_t)1 << p;
	pp[18] = bit_ >> 8;
	pp[19] = bit_;
	pp[20] = 0x88; pp[21] = 0x09;		/* Slow Protocols */
	pp = &uip_buf[TX_BASE + TX_PDU];
	pp[0] = 1;				/* subtype LACP */
	pp[1] = 1;				/* version */
	i_ = PDU_ACTOR;
	tx_info(p);
	i_ = PDU_PARTNER;
	tx_info(p);
	pp[42] = 3;				/* collector information */
	pp[43] = 16;
	/* terminator TLV and padding are zero */
	uip_len = TX_LEN;
	tcpip_output();
	uip_len = 0;
	tx_cnt[p]++;
}


void lacp_in(void) __banked
{
	static __xdata uint8_t p, changed, st;

	p = uip_buf[RX_PORT] & 0x0f;
	bit_ = (uint16_t)1 << p;
	if (uip_len < RX_PDU + PDU_PARTNER + I_STATE + 1 || p >= LACP_PORTS
	    || !(lacp_ports & bit_)
	    || uip_buf[RX_ETYPE] != 0x88 || uip_buf[RX_ETYPE + 1] != 0x09
	    || uip_buf[RX_PDU] != 1 || !uip_buf[RX_PDU + 1]
	    || uip_buf[RX_PDU + PDU_ACTOR - 2] != 1 || uip_buf[RX_PDU + PDU_PARTNER - 2] != 2) {
		rx_ignored++;
		for (i_ = 0; i_ < 8; i_++)
			rx_last[i_] = uip_buf[RX_PORT - 1 + i_];
		uip_len = 0;
		return;
	}
	uip_len = 0;
	rx_cnt[p]++;
	pp = &uip_buf[RX_PDU];

	/* the partner: the PDU's actor */
	changed = 0;
	if (p_sysprio[p] != get16(PDU_ACTOR + I_SYSPRIO) || p_key[p] != get16(PDU_ACTOR + I_KEY)
	    || p_pprio[p] != get16(PDU_ACTOR + I_PPRIO) || p_port[p] != get16(PDU_ACTOR + I_PORT))
		changed = 1;
	for (i_ = 0; i_ < 6; i_++)
		if (p_sys[p][i_] != pp[PDU_ACTOR + I_SYS + i_]) {
			changed = 1;
			p_sys[p][i_] = pp[PDU_ACTOR + I_SYS + i_];
		}
	p_sysprio[p] = get16(PDU_ACTOR + I_SYSPRIO);
	p_key[p] = get16(PDU_ACTOR + I_KEY);
	p_pprio[p] = get16(PDU_ACTOR + I_PPRIO);
	p_port[p] = get16(PDU_ACTOR + I_PORT);
	st = lacp_partner_st[p];
	lacp_partner_st[p] = pp[PDU_ACTOR + I_STATE];
	if (changed || st != lacp_partner_st[p])
		ntt[p] = 1;

	/* us, as the partner sees us */
	p_matched[p] = get16(PDU_PARTNER + I_SYSPRIO) == lacp_sysprio
		       && get16(PDU_PARTNER + I_KEY) == lacp_group[p]
		       && get16(PDU_PARTNER + I_PPRIO) == lacp_pprio[p]
		       && get16(PDU_PARTNER + I_PORT) == p + 1
		       && (pp[PDU_PARTNER + I_STATE] & LACP_ST_AGGREGATION);
	for (i_ = 0; i_ < 6; i_++)
		if (pp[PDU_PARTNER + I_SYS + i_] != uip_ethaddr.addr[i_])
			p_matched[p] = 0;
	if (!p_matched[p] || pp[PDU_PARTNER + I_STATE] != lacp_actor_st[p])
		ntt[p] = 1;

	cur_while[p] = lacp_fast[p] ? SHORT_TIMEOUT : LONG_TIMEOUT;
	/* a partner asking for the short timeout gets the fast rate now */
	if ((lacp_partner_st[p] & LACP_ST_TIMEOUT) && per_while[p] > FAST_PERIODIC)
		per_while[p] = FAST_PERIODIC;
	if (changed && (lacp_bundled & bit_)) {
		/* another system: re-select from scratch */
		mask_on = 0;
		mask_set(p);
	}
	eval_all();
}


void lacp_tick(void) __banked
{
	static __xdata uint16_t now;

	if (!lacp_ports)
		return;
	/* same layout as the STP code's link supervision */
	reg_read_m(RTL837X_REG_LINKS_STS);
	now = (uint16_t)sfr_data[1] | ((uint16_t)sfr_data[2] << 8);
	for (lp_ = 0; lp_ < LACP_PORTS; lp_++) {
		bit_ = (uint16_t)1 << lp_;
		if (!(lacp_ports & bit_))
			continue;
		if (!(now & bit_)) {
			if (links & bit_)
				partner_clear(lp_);
			per_while[lp_] = 0;
			continue;
		}
		if (cur_while[lp_] && !--cur_while[lp_])
			lacp_partner_st[lp_] = 0;	/* expired: slow periodic until heard again */
		if (per_while[lp_])
			per_while[lp_]--;
		if (!per_while[lp_]) {
			if (lacp_mode[lp_] == LACP_MODE_ACTIVE || (lacp_partner_st[lp_] & LACP_ST_ACTIVITY))
				ntt[lp_] = 1;
			per_while[lp_] = (lacp_partner_st[lp_] & LACP_ST_TIMEOUT) ? FAST_PERIODIC : SLOW_PERIODIC;
		}
	}
	links = now;
	eval_all();
	for (lp_ = 0; lp_ < LACP_PORTS; lp_++) {
		bit_ = (uint16_t)1 << lp_;
		if (!ntt[lp_] || !(lacp_ports & bit_) || !(links & bit_))
			continue;
		ntt[lp_] = 0;
		/* a passive port speaks only when spoken to */
		if (lacp_mode[lp_] != LACP_MODE_ACTIVE && !(lacp_partner_st[lp_] & LACP_ST_ACTIVITY)
		    && !cur_while[lp_])
			continue;
		lacp_tx(lp_);
	}
}


/* ---- show lacp ---- */

static __xdata uint8_t col;

static void o_c(char c)
{
	write_char(c);
	col++;
}

static void o_s(__code const char *s)
{
	static __code const char * __xdata sp;

	sp = s;
	while (*sp)
		o_c(*sp++);
}

/* the value to print is in ov: a parameter live across calls would need
 * internal RAM */
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

static void o_hex(void)
{
	o_c("0123456789abcdef"[(uint8_t)ov >> 4]);
	o_c("0123456789abcdef"[ov & 15]);
}

static void o_to(uint8_t c)
{
	static __xdata uint8_t to;

	to = c;
	do
		o_c(' ');
	while (col < to);
}

static void o_mac(__xdata uint8_t *m)
{
	static __xdata uint8_t * __xdata mp;

	mp = m;
	for (g_ = 0; g_ < 6; g_++) {
		ov = mp[g_];
		o_hex();
		if (g_ == 1 || g_ == 3)
			o_c('.');
	}
}

static __code const char * __code const why_txt[] = {
	"down", "no partner", "other system", "waiting", "min-links", "bundled"
};


void lacp_show(void) __banked
{
	col = 0;
	o_s("System: ");
	ov = lacp_sysprio;
	o_dec();
	o_c(',');
	o_mac(uip_ethaddr.addr);
	if (rx_ignored) {
		o_s("  ignored: ");
		ov = rx_ignored;
		o_dec();
		o_s(" (");
		for (lp_ = 0; lp_ < 8; lp_++) {
			ov = rx_last[lp_];
			o_hex();
		}
		o_c(')');
	}
	write_char('\n');
	if (!lacp_ports) {
		print_string("No LACP ports\n");
		return;
	}
	print_string("Port    Group  Mode     State         Partner system        Key    Port   Rx     Tx\n"
		     "------  -----  -------  ------------  --------------------  -----  -----  -----  -----\n");
	for (lp_ = machine.min_port; lp_ <= machine.max_port; lp_++) {
		if (!(lacp_ports & ((uint16_t)1 << lp_)))
			continue;
		col = 0;
		o_s("Eth1/");
		ov = machine.log_to_phys_port[lp_];
		o_dec();
		o_to(8);
		o_s("Po");
		ov = lacp_group[lp_];
		o_dec();
		o_to(15);
		o_s(lacp_mode[lp_] == LACP_MODE_ACTIVE ? "active" : "passive");
		if (lacp_fast[lp_])
			o_c('*');
		o_to(24);
		o_s(why_txt[why[lp_]]);
		o_to(38);
		if (cur_while[lp_]) {
			ov = p_sysprio[lp_];
			o_dec();
			o_c(',');
			o_mac(p_sys[lp_]);
			o_to(60);
			ov = p_key[lp_];
			o_dec();
			o_to(67);
			ov = p_port[lp_];
			o_dec();
		} else {
			o_c('-');
			o_to(60);
			o_c('-');
			o_to(67);
			o_c('-');
		}
		o_to(74);
		ov = rx_cnt[lp_];
		o_dec();
		o_to(81);
		ov = tx_cnt[lp_];
		o_dec();
		write_char('\n');
	}
	print_string("* = lacp rate fast\n");
}
