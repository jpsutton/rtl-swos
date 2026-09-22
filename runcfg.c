/*
 * Running configuration renderer. See runcfg.h.
 *
 * One emitter feeds two sinks: the console (show running-config) and
 * cfg_buf (write memory). Everything is read from the live state, so a
 * setting made by a legacy command that still runs through the old
 * parser is rendered as well; only settings without any readable state
 * (speed, shutdown, IGMP snooping) come from the swcfg shadows.
 *
 * IPv4 addresses are read byte-wise: uIP keeps them in network order in
 * memory. uIP's own uip_ipaddrN() accessors call htons(), which lives in
 * BANK1 and is not __banked, so they must not be used from here.
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_flash.h"
#include "rtl837x_phy.h"
#include "rtl837x_port.h"
#include "machine.h"
#include "dhcp.h"
#include "syslog.h"
#include "telnetd.h"
#include "tftp.h"
#include "uip/uip.h"
#include "swcfg.h"
#include "runcfg.h"

#pragma codeseg BANK3
#pragma constseg BANK3

extern __code const struct machine machine;
extern __xdata uint16_t management_vlan;
extern __xdata uint8_t vlan_names[VLAN_NAMES_SIZE];
extern __xdata struct dhcp_state dhcp_state;
extern __xdata char passwd[21];
extern __xdata uint8_t sfr_data[4];
extern __xdata uint8_t flash_buf[FLASH_BUF_SIZE];
extern __xdata struct flash_region_t flash_region;

/* Scratch only: filled before every use, so it may live above XRAM_LOW_LIMIT */
__xdata __at(XRAM_CFG_BUF) uint8_t cfg_buf[CONFIG_LEN];

static __xdata uint8_t rc_tobuf;	/* 1: render into cfg_buf */
static __xdata uint16_t rc_len;
static __xdata uint8_t rc_over;

static void rc_c(__xdata char c)
{
	if (!rc_tobuf) {
		write_char(c);
		return;
	}
	if (rc_len < CONFIG_LEN - 1)
		cfg_buf[rc_len++] = c;
	else
		rc_over = 1;
}


static void rc_s(__code const char * __xdata s)
{
	while (*s)
		rc_c(*s++);
}


/* xdata string up to NUL, or up to the first space when word is set
 * (the vlan name table separates entries with a space) */
static void rc_x(__xdata const char * __xdata s, __xdata uint8_t word)
{
	while (*s && !(word && *s == ' '))
		rc_c(*s++);
}


static void rc_dec(__xdata uint16_t v)
{
	static __xdata char b[6];
	static __xdata uint8_t n;

	n = 0;
	do {
		b[n++] = '0' + v % 10;
		v /= 10;
	} while (v);
	while (n)
		rc_c(b[--n]);
}


static void rc_ip(__xdata const uint8_t * __xdata a)
{
	rc_dec(a[0]);
	rc_c('.');
	rc_dec(a[1]);
	rc_c('.');
	rc_dec(a[2]);
	rc_c('.');
	rc_dec(a[3]);
}


static uint8_t ip_is_zero(__xdata const uint8_t * __xdata a)
{
	return !(a[0] | a[1] | a[2] | a[3]);
}


static uint8_t passwd_is_default(void)
{
	static __code const char * __xdata d;
	static __xdata uint8_t k;

	d = DEFAULT_PASSWORD;
	for (k = 0; d[k]; k++) {
		if (passwd[k] != d[k])
			return 0;
	}
	return passwd[k] == 0;
}


static void rc_speed(__xdata uint8_t sp)
{
	switch (sp) {
	case PHY_SPEED_10M:
		rc_s("10");
		break;
	case PHY_SPEED_100M:
		rc_s("100");
		break;
	case PHY_SPEED_1G:
		rc_s("1000");
		break;
	case PHY_SPEED_2G5:
		rc_s("2500");
		break;
	case PHY_SPEED_5G:
		rc_s("5000");
		break;
	case PHY_SPEED_10G:
		rc_s("10000");
		break;
	}
}


static void rc_vlans(void)
{
	static __xdata uint16_t last, next, vid, n;
	static __xdata uint8_t k, any;

	/* ascending VID order, independent of the database slot order */
	any = 0;
	last = 0;
	while (1) {
		next = 0xffff;
		for (k = 0; k < SW_MAX_VLANS; k++) {
			vid = sw_vlans[k];
			if (vid > last && vid < next)
				next = vid;
		}
		if (next == 0xffff)
			break;
		last = next;
		n = vlan_name(next);
		if (next == 1 && n == 0xffff)
			continue;	/* the default VLAN, unnamed */
		rc_s("vlan ");
		rc_dec(next);
		rc_c('\n');
		if (n != 0xffff) {
			rc_s(" name ");
			rc_x((__xdata char *)&vlan_names[n], 1);
			rc_c('\n');
		}
		any = 1;
	}
	if (any)
		rc_s("!\n");
}


static void rc_allowed(__xdata struct sw_port * __xdata sp)
{
	static __xdata uint8_t r;

	if (!sp->nranges) {
		rc_s("none");
		return;
	}
	for (r = 0; r < sp->nranges; r++) {
		if (r)
			rc_c(',');
		rc_dec(sp->allowed[r].lo);
		if (sp->allowed[r].hi != sp->allowed[r].lo) {
			rc_c('-');
			rc_dec(sp->allowed[r].hi);
		}
	}
}


static void rc_interfaces(void)
{
	static __xdata uint8_t up, lp;
	static __xdata uint16_t mtu;
	static __xdata struct sw_port * __xdata sp;

	for (up = 1; up <= 9; up++) {
		lp = machine.phys_to_log_port[up - 1];
		if (lp < machine.min_port || lp > machine.max_port)
			continue;
		sp = &sw_ports[lp];
		rc_s("interface ethernet 1/");
		rc_dec(up);
		rc_c('\n');
		if (port_names[lp][0]) {
			rc_s(" description ");
			rc_x(port_names[lp], 0);
			rc_c('\n');
		}
		if (sp->shut)
			rc_s(" shutdown\n");
		if (sp->speed != PHY_SPEED_AUTO) {
			rc_s(" speed ");
			rc_speed(sp->speed);
			rc_c('\n');
		}
		reg_read_m(RTL8373_REG_MAC_L2_PORT_MAX_LEN + ((uint16_t)lp << 8));
		mtu = (((uint16_t)sfr_data[2] << 8) | sfr_data[3]) & 0x3fff;
		if (mtu != 0x3fff) {
			rc_s(" mtu ");
			rc_dec(mtu);
			rc_c('\n');
		}
		if (sp->mode == SW_MODE_TRUNK) {
			rc_s(" switchport mode trunk\n");
			if (sp->native_vid != 1) {
				rc_s(" switchport trunk native vlan ");
				rc_dec(sp->native_vid);
				rc_c('\n');
			}
			if (!(sp->nranges == 1 && sp->allowed[0].lo == 1
			      && sp->allowed[0].hi == SW_VID_MAX)) {
				rc_s(" switchport trunk allowed vlan ");
				rc_allowed(sp);
				rc_c('\n');
			}
		} else if (sp->access_vid != 1) {
			rc_s(" switchport access vlan ");
			rc_dec(sp->access_vid);
			rc_c('\n');
		}
		rc_s("!\n");
	}
}


static void rc_emit(void)
{
	rc_s("!\nhostname ");
	rc_x(hostname, 0);
	rc_s("\n!\n");

	rc_vlans();
	rc_interfaces();

	if (management_vlan) {
		rc_s("interface vlan ");
		rc_dec(management_vlan);
		rc_c('\n');
		if (dhcp_state.state != DHCP_OFF) {
			rc_s(" ip address dhcp\n");
		} else {
			rc_s(" ip address ");
			rc_ip((__xdata uint8_t *)uip_hostaddr);
			rc_c(' ');
			rc_ip((__xdata uint8_t *)uip_netmask);
			rc_c('\n');
		}
		rc_s("!\n");
	}

	if (dhcp_state.state == DHCP_OFF && !ip_is_zero((__xdata uint8_t *)uip_draddr)) {
		rc_s("ip default-gateway ");
		rc_ip((__xdata uint8_t *)uip_draddr);
		rc_c('\n');
	}
	if (sw_igmp)
		rc_s("ip igmp snooping\n");
	if (syslog_state.enabled) {
		rc_s("logging host ");
		rc_ip(syslog_state.server_ip);
		if (syslog_state.server_port != SYSLOG_PORT_DEFAULT) {
			rc_s(" port ");
			rc_dec(syslog_state.server_port);
		}
		rc_c('\n');
	}
	if (telnet_state.enabled)
		rc_s("feature telnet\n");

	if (telnet_state.idle_secs != TELNET_IDLE_DEFAULT || !passwd_is_default()) {
		rc_s("!\nline vty\n");
		if (telnet_state.idle_secs == 0xffff) {
			rc_s(" exec-timeout 0 0\n");
		} else if (telnet_state.idle_secs != TELNET_IDLE_DEFAULT) {
			rc_s(" exec-timeout ");
			rc_dec(telnet_state.idle_secs / 60);
			rc_c(' ');
			rc_dec(telnet_state.idle_secs % 60);
			rc_c('\n');
		}
		if (!passwd_is_default()) {
			rc_s(" password ");
			rc_x(passwd, 0);
			rc_c('\n');
		}
	}
	rc_s("!\nend\n");
}


void runcfg_show(void) __banked
{
	rc_tobuf = 0;
	rc_emit();
}


uint16_t runcfg_render(void) __banked
{
	rc_tobuf = 1;
	rc_len = 0;
	rc_over = 0;
	rc_emit();
	rc_tobuf = 0;
	if (rc_over)
		return 0xffff;
	cfg_buf[rc_len] = 0;
	return rc_len;
}


void runcfg_save(void) __banked
{
	static __xdata uint16_t n;

	/* A running TFTP transfer owns cfg_buf and may touch the sector */
	if (tftp_busy()) {
		print_string("% TFTP transfer in progress, try again later\n");
		return;
	}
	print_string("Building configuration...\n");
	n = runcfg_render();
	if (n == 0xffff) {
		print_string("% Configuration exceeds the flash sector, not saved\n");
		return;
	}
	flash_region.addr = CONFIG_START;
	flash_sector_erase();
	flash_region.addr = CONFIG_START;
	flash_region.len = n + 1;
	flash_write_bytes(cfg_buf);
	print_string("[OK]\n");
}


void startup_show(void) __banked
{
	static __xdata uint32_t pos;
	static __xdata uint16_t i;
	static __xdata uint8_t c;

	for (pos = CONFIG_START; pos < CONFIG_START + CONFIG_LEN; pos += FLASH_BUF_SIZE) {
		flash_region.addr = pos;
		flash_region.len = FLASH_BUF_SIZE;
		flash_read_bulk(flash_buf);
		for (i = 0; i < FLASH_BUF_SIZE; i++) {
			c = flash_buf[i];
			if (c == 0 || c == 0xff)
				return;
			write_char(c);
		}
	}
}
