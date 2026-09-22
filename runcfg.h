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
extern __xdata uint8_t cfg_buf[CONFIG_LEN];

void runcfg_show(void) __banked;	/* show running-config */
void runcfg_save(void) __banked;	/* write memory */
void startup_show(void) __banked;	/* show startup-config */

/* Render into cfg_buf; returns the length, or 0xffff if it did not fit */
uint16_t runcfg_render(void) __banked;

#endif
