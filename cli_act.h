#ifndef __CLI_ACT_H__
#define __CLI_ACT_H__

#include <stdint.h>

/*
 * Command actions of the modal CLI. The engine (cli.c, BANK1) walks the
 * command tree and hands the action id of the matched node to cli_act()
 * (cli_act.c, BANK3), which runs it. cli.lo carries the matched node's
 * ->lo, cli.line the line.
 */
/* ---------------- actions ---------------- */
#define ACT_NONE	0
#define ACT_ENABLE	1
#define ACT_DISABLE	2
#define ACT_CONF_T	3
#define ACT_EXIT	4
#define ACT_END		5
#define ACT_WRITE	7
#define ACT_RELOAD	8
#define ACT_IF		9
#define ACT_VLAN	10
#define ACT_SHUT	12	/* interface: shutdown / no shutdown */
#define ACT_SPEED	13	/* interface: speed <val> (value in node->lo) */
#define ACT_DESC	14	/* interface: description LINE / no description */
#define ACT_SVI		15	/* interface vlan N */
#define ACT_VLAN_NAME	16
#define ACT_SW_MODE	17	/* mode in node->lo */
#define ACT_SW_ACCESS	18
#define ACT_SW_NATIVE	19
#define ACT_SW_ALLOWED	20	/* operation in node->lo */
#define ACT_MTU		21
#define ACT_HOSTNAME	22
#define ACT_IP_ADDR	23
#define ACT_IP_DHCP	24
#define ACT_DEFGW	25
#define ACT_IGMP	26
#define ACT_LOG_HOST	27
#define ACT_SHOW_RUN	28
#define ACT_SHOW_START	29
#define ACT_FEAT_TELNET	30
#define ACT_LINE_VTY	31
#define ACT_EXEC_TO	32
#define ACT_VTY_PW	33
#define ACT_EEE		34
#define ACT_PROT	35
#define ACT_RL		36	/* ->lo: 1 input, 2 output, 3 input+drop */
#define ACT_CHGRP	37
#define ACT_PO		38	/* interface port-channel N */
#define ACT_LB		39	/* load-balance: fields accumulated in cli.acc */
#define ACT_MON_SRC	40	/* ->lo: 1 rx, 2 tx, 3 both */
#define ACT_MON_DST	41
#define ACT_MON_DEL	42
#define ACT_FEAT_STP	43
#define ACT_STP_G	44	/* global spanning-tree, parameter in ->lo */
#define ACT_STP_IF	45	/* per-port spanning-tree, parameter in ->lo */
#define ACT_SHOW	46	/* operational show commands, SHOW_* in ->lo */
#define ACT_CLEAR_MAC	47
#define ACT_DEBUG	48	/* DBG_* in ->lo */
#define ACT_COPY	49	/* TFTP_OP_* in ->lo */
#define ACT_DUPLEX	50	/* PHY_DUPLEX_* in ->lo */
#define ACT_MACADDR	51
#define ACT_MROUTER	52	/* interface: ip igmp snooping mrouter */
#define ACT_LACP	53	/* LACPC_* in ->lo */
#define ACT_PC_LB	54	/* global port-channel load-balance, hash bits in ->lo */
#define ACT_NAMESERVER	55
#define ACT_NSLOOKUP	56
#define ACT_NTP_SERVER	57
#define ACT_CLOCK_TZ	58	/* clock timezone NAME HOURS [MINUTES] */
#define ACT_CLOCK_ST	59	/* clock summer-time NAME recurring [eu|us]; NTP_DST_* in ->lo */
#define ACT_TOTP	60	/* TOTPC_* in ->lo */
#define ACT_COPY_SR	61	/* copy startup-config running-config */
#define ACT_SHOW_RUNF	62	/* filtered show running-config, RCF_* in ->lo */
#define ACT_SRUN_PO	63
#define ACT_SRUN_SVI	64
#define ACT_SRUN_VLAN	65
#define ACT_PING	66	/* options in cli.acc: 1 repeat, 2 size */
#define ACT_CLEAR_LOG	67
#define ACT_LLDP	68	/* LLDPC_* in ->lo */
#define LLDPC_FEATURE	1
#define LLDPC_TX	2
#define LLDPC_RX	3
#define TOTPC_SECRET	1
#define TOTPC_LOGIN	2

#define SHOW_IF_STATUS	1
#define SHOW_IF_COUNT	2
#define SHOW_IF_TRUNK	3
#define SHOW_IF_XCVR	4
#define SHOW_VLAN	5
#define SHOW_PO		6
#define SHOW_IP_IF	7
#define SHOW_MON	8
#define SHOW_MAC	9
#define SHOW_STP	10
#define SHOW_TFTP	11
#define SHOW_VER	12
#define SHOW_HIST	13
#define SHOW_LOG	14
#define SHOW_IGMP	15
#define SHOW_LACP	16
#define SHOW_HOSTS	17
#define SHOW_CLOCK	18
#define SHOW_NTP	19
#define SHOW_TOTP	20
#define SHOW_IF_DETAIL	21
#define SHOW_LLDP	22
#define SHOW_LLDP_D	23

/* ACT_STP_G parameters */
#define STPG_RSTP	1
#define STPG_STP	2
#define STPG_PRIO	3
#define STPG_HELLO	4
#define STPG_FWD	5
#define STPG_MAXAGE	6
#define STPG_TXHOLD	7
/* ACT_STP_IF parameters */
#define STPI_PORTFAST	1
#define STPI_PF_DIS	2
#define STPI_BPDUGUARD	3
#define STPI_BPDUFILT	4
#define STPI_ROOTGUARD	5
#define STPI_COST	6
#define STPI_PPRIO	7
#define STPI_P2P	8
#define STPI_SHARED	9
#define STPI_DISABLE	10
/* ACT_LACP parameters */
#define LACPC_FAST	1	/* lacp rate fast|normal */
#define LACPC_PPRIO	2	/* lacp port-priority N */
#define LACPC_SYSPRIO	3	/* lacp system-priority N */
#define LACPC_MINLINKS	4	/* lacp min-links N */
#define LACPC_NORMAL	5	/* lacp rate normal, no lacp rate */

void cli_act(uint8_t action) __banked;
/* Run the action on every port of cli.ctx_range. */
void cli_act_range(uint8_t action) __banked;

#endif
