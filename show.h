#ifndef __SHOW_H__
#define __SHOW_H__

void show_if_status(void) __banked;	/* show interfaces status */
void show_if_counters(void) __banked;	/* show interfaces counters */
void show_if_trunk(void) __banked;	/* show interfaces trunk */
void show_if_transceiver(void) __banked;	/* show interfaces transceiver */
void show_vlan_brief(void) __banked;	/* show vlan brief */
void show_po_summary(void) __banked;	/* show port-channel summary */
void show_ip_if_brief(void) __banked;	/* show ip interface brief */
void show_monitor(void) __banked;	/* show monitor session */
void show_mac_table(void) __banked;	/* show mac address-table */
void show_stp(void) __banked;		/* show spanning-tree */

#endif
