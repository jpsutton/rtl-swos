/*
 * debug ... : raw access to the chip for development - switch registers,
 * SerDes, PHYs, XRAM, GPIOs, the RNG and the flash IDs. Arguments come
 * parsed from the CLI engine (cli.args). Locals live in xdata, see cli.c.
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_regs.h"
#include "rtl837x_sfr.h"
#include "rtl837x_flash.h"
#include "cli.h"
#include "dbgcmd.h"
#include "bank4.h"

#pragma codeseg BANK3
#pragma constseg BANK3

extern __xdata uint8_t sfr_data[4];
extern __xdata struct flash_region_t flash_region;

/* an XRAM address as a pointer; meaningless (and never exercised) on the
 * host test build, where pointers are wider than 16 bits */
#ifdef SWOS_HOST_TEST
#include <stdint.h>
#define XPTR(a) ((uint8_t *)(uintptr_t)(a))
#else
#define XPTR(a) ((__xdata uint8_t *)(a))
#endif

static __xdata uint8_t dbg_gpio_last[8];

static void dbg_gpio(void)
{
	static __xdata uint8_t idx, k, v;

	for (idx = 0; idx < 2; idx++) {
		reg_read_m(RTL837X_REG_GPIO_00_31_INPUT + (idx * 4));
		print_string("GPIO ");
		write_char(idx ? '1' : '0');
		print_string(": ");
		print_sfr_data();
		print_string("  changed: ");
		for (k = 0; k < 4; k++) {
			v = sfr_data[k];
			print_byte(dbg_gpio_last[idx * 4 + k] ^ v);
			dbg_gpio_last[idx * 4 + k] = v;
		}
		write_char('\n');
	}
}


static void dbg_security(__xdata uint32_t a)
{
	flash_region.addr = a;
	flash_region.len = 40;
	flash_read_security();
	write_char('\n');
}


void debug_run(uint8_t op) __banked
{
	static __xdata uint8_t o, k, pass;
	static __xdata uint16_t a, n, i, bad;
	static __xdata uint8_t * __xdata x;

	o = op;
	a = cli.args[0];
	switch (o) {
	case DBG_REG_RD:
		print_short(a);
		print_string(": ");
		reg_read_m(a);
		print_sfr_data();
		write_char('\n');
		break;
	case DBG_REG_WR:
		sfr_data[0] = cli.args[1] >> 24;
		sfr_data[1] = cli.args[1] >> 16;
		sfr_data[2] = cli.args[1] >> 8;
		sfr_data[3] = cli.args[1];
		reg_write_m(a);
		print_short(a);
		print_string(" <- ");
		print_sfr_data();
		write_char('\n');
		break;
	case DBG_SDS_RD:
		sds_read(cli.args[0], cli.args[1], cli.args[2]);
		print_phy_data();
		write_char('\n');
		break;
	case DBG_SDS_WR:
		sds_write_v(cli.args[0], cli.args[1], cli.args[2], cli.args[3]);
		break;
	case DBG_PHY_RD:
		phy_read(cli.args[0], cli.args[1], cli.args[2]);
		print_phy_data();
		write_char('\n');
		break;
	case DBG_PHY_WR:
		phy_write(cli.args[0], cli.args[1], cli.args[2], cli.args[3]);
		break;
	case DBG_X_RD:
		x = XPTR(a);
		print_short(a);
		write_char(':');
		for (k = 0; k < 16; k++) {
			write_char(' ');
			print_byte(x[k]);
		}
		write_char('\n');
		break;
	case DBG_X_TEST:
		/* destructive: only for the scratch region above the limit */
		n = cli.args[1];
		if (a < XRAM_LOW_LIMIT) {
			print_string("% Refusing to test live variables below 0x4000\n");
			break;
		}
		x = XPTR(a);
		bad = 0;
		for (pass = 0; pass < 3; pass++) {
			for (i = 0; i < n; i++)
				x[i] = pass == 0 ? 0x55 : pass == 1 ? 0xaa : (uint8_t)(i ^ (i >> 8));
			for (i = 0; i < n; i++) {
				if (x[i] != (pass == 0 ? 0x55 : pass == 1 ? 0xaa : (uint8_t)(i ^ (i >> 8))))
					bad++;
			}
		}
		print_string("Bad bytes: ");
		itoa_short(bad);
		write_char('\n');
		break;
	case DBG_GPIO:
		dbg_gpio();
		break;
	case DBG_BANK4:
		print_string("Calling into BANK4... ");
		print_short(bank4_probe());
		write_char('\n');
		break;
	case DBG_RND:
		/* the enable bit has to be set again for every new number */
		reg_bit_set(RTL837X_RLDP_RLPP, RLDP_RND_EN);
		reg_read_m(RTL837X_RAND_NUM1);
		print_sfr_data();
		write_char('\n');
		break;
	case DBG_FL_ID:
		flash_read_jedecid();
		write_char('\n');
		break;
	case DBG_FL_UID:
		print_string("(only 4 bytes are likely valid)\n");
		flash_read_uid();
		write_char('\n');
		break;
	case DBG_FL_SEC:
		/* only a managed switch has these programmed */
		dbg_security(0x1000);
		dbg_security(0x2000);
		dbg_security(0x3000);
		break;
	}
}
