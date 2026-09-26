/*
 * env_cli.c - the cli.c / swcfg.c edges that are not the port driver:
 * the hostname, the bridged actions, the PHY,
 * IGMP, DHCP and syslog entry points, all recorded so tests can assert on
 * them. Linked by every test binary together with env_tables.c.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rtl837x_common.h"
#include "rtl837x_phy.h"
#include "uip.h"
#include "dhcp.h"
#include "syslog.h"
#include "telnetd.h"
#include "rtl837x_flash.h"
#include "support.h"

char hostname[24] = "sw";

int n_save, n_reset;
int n_igmp_on, n_igmp_off, n_hostdef, n_dhcp_start, n_dhcp_stop;
int n_syslog_start, n_syslog_stop;
struct dhcp_state dhcp_state;

/* phy driver edges the interface handlers touch */
struct phy_settings phy_settings;
char port_names[9][PORT_NAME_SIZE];
int n_setspeed;
uint8_t last_speed, last_port;

void phy_set_speed(void)
{
	n_setspeed++;
	last_speed = phy_settings.speed;
	last_port = phy_settings.port;
}

extern uint16_t igmp_mrouter;
void env_cli_reset(void)
{
	igmp_mrouter = 0;
	n_save = n_reset = n_setspeed = 0;
	n_igmp_on = n_igmp_off = n_hostdef = n_dhcp_start = n_dhcp_stop = 0;
	n_syslog_start = n_syslog_stop = 0;
	syslog_state.enabled = 0;
	dhcp_state.state = 0;
	last_speed = last_port = 0;
	for (int i = 0; i < 9; i++)
		port_names[i][0] = 0;
}

void itoa_short(uint16_t v)
{
	char b[6];
	int n = 0;

	do {
		b[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n)
		write_char(b[--n]);
}



void reset_chip(void)
{
	n_reset++;
}

void igmp_enable(void) { n_igmp_on++; }
void igmp_setup(void) { n_igmp_off++; }
uint16_t igmp_mrouter;
void igmp_router_port_set(uint16_t pmask) { igmp_mrouter = pmask; }

/* boot.c: derives a name from the MAC when none is configured */
void set_hostname_default(void)
{
	n_hostdef++;
	if (!hostname[0]) {
		const char *d = "default";
		int i = 0;
		while (d[i]) {
			hostname[i] = d[i];
			i++;
		}
		hostname[i] = 0;
	}
}

void dhcp_start(void) { n_dhcp_start++; dhcp_state.state = 1; }
void dhcp_stop(void) { n_dhcp_stop++; dhcp_state.state = 0; }

/* syslog_state itself lives in env_tables.c */
void syslog_start(void) { n_syslog_start++; syslog_state.enabled = 1; }
void syslog_stop(void) { n_syslog_stop++; syslog_state.enabled = 0; }

/* ---- telnet server (telnetd.c is not linked) ---- */
struct telnet_state_t telnet_state = { .idle_secs = TELNET_IDLE_DEFAULT };
char passwd[21] = "1234";
void telnet_start(void) { telnet_state.enabled = 1; }
void telnet_stop(void) { telnet_state.enabled = 0; }
void telnet_set_timeout(uint16_t secs) { telnet_state.idle_secs = secs; }
uint8_t tftp_busy(void) { return 0; }

/* ---- the startup-config sector as a fake flash; everything else reads 0xff ---- */
uint8_t fake_cfg[CONFIG_LEN];
uint8_t flash_buf[FLASH_BUF_SIZE];
extern struct flash_region_t flash_region;

void fake_flash_reset(void)
{
	for (int i = 0; i < CONFIG_LEN; i++)
		fake_cfg[i] = 0xff;
}

void flash_sector_erase(void)
{
	if (flash_region.addr == CONFIG_START)
		fake_flash_reset();
}

void flash_write_bytes(uint8_t *p)
{
	for (uint32_t i = 0; i < flash_region.len; i++) {
		uint32_t a = flash_region.addr + i;
		if (a >= CONFIG_START && a < CONFIG_START + CONFIG_LEN)
			fake_cfg[a - CONFIG_START] &= p[i];	/* NOR: program clears bits */
	}
}

void flash_read_bulk(uint8_t *dst)
{
	for (uint32_t i = 0; i < flash_region.len; i++) {
		uint32_t a = flash_region.addr + i;
		dst[i] = (a >= CONFIG_START && a < CONFIG_START + CONFIG_LEN)
			 ? fake_cfg[a - CONFIG_START] : 0xff;
	}
}

/* ---- STP engine config API (rtl837x_stp.c is not linked; its variables
 * live in env_tables.c) ---- */
#include "rtl837x_stp.h"
#include "rtl837x_port.h"
extern bool stp_enabled;
int n_stp_enable, n_stp_disable, n_stp_prio;

uint8_t stp_cfg_entity(uint8_t port)
{
	uint8_t g = port_lag_of(port);
	return g == PORT_LAG_NONE ? port : STP_LAG_BASE + g;
}
void stp_cfg_enable(uint8_t on)
{
	stp_enabled = on;
	if (on)
		n_stp_enable++;
	else
		n_stp_disable++;
}
void stp_cfg_prio(uint8_t prio) { stp_prio = prio; n_stp_prio++; }
int n_stp_sync;
void stp_cfg_sync(uint8_t ent, uint8_t was_out)
{
	if (was_out != (STP_PF_OUT(stp_pflags[ent]) ? 1 : 0))
		n_stp_sync++;
}

/* what stp_defaults() establishes */
void stp_test_defaults(void)
{
	stp_enabled = 0;
	stp_prio = 0x80;
	stp_hello_s = 2;
	stp_maxage_s = 20;
	stp_fwddelay_s = 15;
	stp_rstp = 1;
	stp_txhold = 6;
	for (int i = 0; i < STP_ENTITIES; i++) {
		stp_pflags[i] = STP_PF_ENABLED | STP_PF_AUTOEDGE;
		stp_pcost[i] = 0;
		stp_pprio[i] = 0x80;
		stp_pp2p[i] = 0;
	}
	n_stp_enable = n_stp_disable = n_stp_prio = 0;
}

/* ---- bandwidth driver (rtl837x_bandwidth.c is not linked) ---- */
uint32_t bw_in[10], bw_out[10];
uint8_t bw_in_drop[10];
void bandwidth_ingress_set(uint8_t p, uint32_t bw) { bw_in[p] = bw; bw_in_drop[p] = 0; }
void bandwidth_ingress_disable(uint8_t p) { bw_in[p] = 0; }
void bandwidth_ingress_drop(uint8_t p) { bw_in_drop[p] = 1; }
void bandwidth_egress_set(uint8_t p, uint32_t bw) { bw_out[p] = bw; }
void bandwidth_egress_disable(uint8_t p) { bw_out[p] = 0; }

/* ---- show hooks into modules not linked here ---- */
int n_sfp_info, n_stp_status;
bool sfp_print_info(uint8_t sfp) { (void)sfp; n_sfp_info++; return false; }
bool sfp_print_measurements(uint8_t sfp) { (void)sfp; return true; }
void stp_status(void) { n_stp_status++; print_string((char *)"STP-STATUS\n"); }
void tftp_show(void) { print_string((char *)"TFTP-SHOW\n"); }

/* ---- debug / copy / SFP / IGMP edges (not linked here) ---- */
extern uint8_t sfr_data[4];
void flash_read_jedecid(void) { }
void flash_read_security(void) { }
void flash_read_uid(void) { }
int n_handle_sfp;
uint8_t sfp_speed[2];
void handle_sfp(void) { n_handle_sfp++; }
int n_igmp_show;
void igmp_show(void) { n_igmp_show++; }
void print_sfr_data(void) { for (int i = 0; i < 4; i++) print_byte(sfr_data[i]); }
void print_phy_data(void) { print_string((char *)"PHYDATA"); }
uint8_t last_sds_id, last_sds_page, last_sds_reg; uint16_t last_sds_val;
void sds_read(uint8_t id, uint8_t page, uint8_t reg) { last_sds_id = id; last_sds_page = page; last_sds_reg = reg; }
void sds_write_v(uint8_t id, uint8_t page, uint8_t reg, uint16_t v)
{ last_sds_id = id; last_sds_page = page; last_sds_reg = reg; last_sds_val = v; }
uint8_t last_tftp_op, last_tftp_srv[4]; char last_tftp_file[64];
void tftp_begin(uint8_t op, const uint8_t *srv, const char *fname)
{
	last_tftp_op = op;
	for (int i = 0; i < 4; i++)
		last_tftp_srv[i] = srv[i];
	int n = 0;
	while (fname[n] && fname[n] != ' ' && n < 63) {
		last_tftp_file[n] = fname[n];
		n++;
	}
	last_tftp_file[n] = 0;
}

/* ---- frame I/O for lacp.c: tcpip_output() records what was sent ---- */
uint8_t uip_buf[UIP_CONF_BUFFER_SIZE + 2];
u16_t uip_len;
int n_tx_frames;
uint8_t tx_frames[16][160];	/* ring of the last frames sent, from the TX descriptor on */
void tcpip_output(void)
{
	memcpy(tx_frames[n_tx_frames & 15], uip_buf, sizeof(tx_frames[0]));
	n_tx_frames++;
}

/* ---- telnetd.c is not linked: its session history ---- */
uint8_t telnet_capture;
int n_tn_hist_show;
void telnet_history_show(void) { n_tn_hist_show++; }

/* ---- dns.c is not linked: its state and entry points ---- */
#include "dns.h"
struct dns_state dns_state;
int n_dns_lookup, n_dns_show;
void dns_lookup(void) { n_dns_lookup++; dns_state.status = DNS_PENDING; }
void dns_show(void) { n_dns_show++; }
void print_ip(uint8_t *a)
{
	char b[20];
	snprintf(b, sizeof(b), "%d.%d.%d.%d", a[0], a[1], a[2], a[3]);
	print_string_x(b);
}

/* ---- ntp.c is not linked: its state and entry points ---- */
#include "ntp.h"
struct ntp_state ntp_state = { .tz_name = "UTC" };
int n_ntp_start, n_ntp_stop;
void ntp_start(void) { n_ntp_start++; ntp_state.enabled = 1; }
void ntp_stop(void) { n_ntp_stop++; ntp_state.enabled = 0; }
void ntp_show(void) { print_string("NTP\n"); }
void ntp_show_time(void) { print_string("CLOCK\n"); }

/* ---- totp.c: the real one would need the clock; its state ---- */
#include "totp.h"
uint8_t totp_enabled, totp_keylen;
char totp_b32[TOTP_B32_MAX + 1];
uint32_t ntp_unix_now(void) { return 0; }
uint8_t totp_set_secret(uint8_t *b32)
{
	size_t n = strlen((char *)b32);
	if (n < 16 || n > 51)
		return 0;
	totp_keylen = n * 5 / 8;
	return 1;
}
void totp_status_print(void) { print_string("TOTP\n"); }

/* ---- console.c is not linked: config_merge() over the fake sector ---- */
#include "cli.h"
void config_merge(void)
{
	char line[CMD_BUF_SIZE];
	int n = 0;

	cli_merge_begin();
	for (int i = 0; i < CONFIG_LEN; i++) {
		uint8_t c = fake_cfg[i];
		if (c == '\n' || c == 0 || c == 0xff) {
			line[n] = 0;
			if (n)
				cli_replay_line(line);
			n = 0;
			if (c != '\n')
				break;
			continue;
		}
		if (n < CMD_BUF_SIZE - 1)
			line[n++] = c;
	}
	cli_merge_end();
}

/* ---- ping.c is not linked: record the start ---- */
#include "ping.h"
uint8_t ping_phase, ping_owner;
char ping_host[64];
uint16_t ping_count, ping_size;
void ping_start(char *host, uint16_t n, uint16_t len)
{
	int i;
	for (i = 0; host[i] && host[i] != ' ' && i < 63; i++)
		ping_host[i] = host[i];
	ping_host[i] = 0;
	ping_count = n;
	ping_size = len;
}
uint8_t ntp_local_now(void) { return 0; }
uint16_t ntp_year;
uint8_t ntp_mon, ntp_mday, ntp_hour, ntp_min, ntp_sec;

/* ---- errdisable recovery (rtl837x_stp.c is not linked) ---- */
uint8_t stp_errdis_on;
uint16_t stp_errdis_int = 300;
void stp_err_clear(uint8_t ent) { stp_pflags[ent] &= ~STP_PF_TRIPPED; }
