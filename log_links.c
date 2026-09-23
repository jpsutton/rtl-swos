/*
 * Link supervision for the event log (BANK4): see log_links() in log.h.
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_port.h"
#include "machine.h"
#include "log.h"

extern __code const struct machine machine;

#pragma codeseg BANK4
#pragma constseg BANK4

static __xdata uint16_t last_up;
static __xdata uint8_t links_seeded;

void log_links(void) __banked
{
	static __xdata uint8_t lp, lc;
	static __xdata uint16_t now, bit;

	now = 0;
	for (lp = machine.min_port; lp <= machine.max_port; lp++)
		if (port_link_code(lp) != PORT_LINK_DOWN)
			now |= (uint16_t)1 << lp;
	if (!links_seeded) {	/* the ports that are up at boot are not news */
		links_seeded = 1;
		last_up = now;
		return;
	}
	for (lp = machine.min_port; lp <= machine.max_port; lp++) {
		bit = (uint16_t)1 << lp;
		if ((now ^ last_up) & bit) {
			log_begin("LINK-3-UPDOWN");
			log_s("Interface ");
			log_if(lp);
			if (now & bit) {
				log_s(", changed state to up (");
				lc = port_link_code(lp);
				log_s(lc == PORT_LINK_10M ? "10" : lc == PORT_LINK_100M ? "100" : lc == PORT_LINK_1G ? "1000"
				      : lc == PORT_LINK_2G5 ? "2500" : lc == PORT_LINK_5G ? "5000" : "10000");
				log_s(" Mb/s)");
			} else {
				log_s(", changed state to down");
			}
			log_end();
		}
	}
	last_up = now;
}
