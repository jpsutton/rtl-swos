#ifndef __RUNCFG_H__
#define __RUNCFG_H__

#include <stdint.h>
#include "rtl837x_common.h"

/*
 * Running configuration: rendered from the live state (swcfg, the
 * management interface, the services) in the block syntax the CLI
 * parses, printing only settings that differ from the defaults.
 */

/* Scratch buffer holding one full startup-config sector; shared by
 * write memory and the TFTP config transfer. */
extern __xdata __at(XRAM_CFG_BUF) uint8_t cfg_buf[CONFIG_LEN];
#if XRAM_CFG_BUF + CONFIG_LEN > XRAM_TELNET_OUTBUF
#error "cfg_buf overlaps telnet_outbuf"
#endif

void runcfg_show(void) __banked;	/* show running-config */

/* show running-config interface ... / vlan ...: runcfg_show() prints only
 * the blocks the filter names, then clears it. Saving never filters. */
#define RCF_ALL		0
#define RCF_IF		1	/* every interface block */
#define RCF_ETH		2	/* ethernet ports in rcf_id (bit N = port 1/N) */
#define RCF_PO		3	/* port-channel rcf_id */
#define RCF_SVI		4	/* interface vlan rcf_id */
#define RCF_VLAN	5	/* vlan rcf_id, 0 = all */
extern __xdata uint8_t rcf_kind;
extern __xdata uint16_t rcf_id;
void runcfg_save(void) __banked;	/* write memory */
void startup_show(void) __banked;	/* show startup-config */

/* Render into cfg_buf; returns the length, or 0xffff if it did not fit */
uint16_t runcfg_render(void) __banked;

#endif
