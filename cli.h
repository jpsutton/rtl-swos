#ifndef __CLI_H__
#define __CLI_H__

#include <stdint.h>

/*
 * Modal CLI engine: an industry-standard mode/submode command line
 * (user EXEC > privileged EXEC # global config and submodes) driven by
 * a command tree in code space.
 *
 * Matching rules:
 *  - unique-prefix abbreviation ("conf t", "sh ver")
 *  - literal tokens win over argument placeholders
 *  - `no` prefix inverts a command that allows it
 *  - EXEC commands are reachable from any config mode without `do`
 *
 * `?` lists the candidates at the cursor, Tab completes a unique
 * prefix; both are fed by the line editors through cli_help() /
 * cli_complete().
 */

/* Modes */
#define CLI_MODE_EXEC	0	/* user EXEC:  > */
#define CLI_MODE_PRIV	1	/* privileged EXEC:  # */
#define CLI_MODE_CONFIG	2	/* (config)# */
#define CLI_MODE_IF	3	/* (config-if)# */
#define CLI_MODE_VLAN	4	/* (config-vlan)# */
#define CLI_MODE_LINE	5	/* (config-line)# */
#define CLI_MODE_SVI	6	/* (config-if)# on interface vlan N */
#define CLI_MODE_PO	7	/* (config-if)# on interface port-channel N */

/* Node flags */
#define CLI_F_PRIV	0x01	/* hidden and refused in user EXEC */
#define CLI_F_NO_OK	0x02	/* usable under `no` */
#define CLI_F_NO_ONLY	0x04	/* only usable under `no` */
#define CLI_F_NO_EXEC	0x08	/* executable here only under `no`; plain form is incomplete */
#define CLI_F_ACC	0x10	/* literal ORs its ->lo into cli.acc when matched (word lists) */

/* Argument placeholder types (a node with word == 0 is an argument) */
#define CLI_A_NONE	0
/* NOTE: a CLI_A_NUM node's lo/hi are its range, so a handler that reads
 * cli.lo must not be reachable through one - use CLI_A_NUM32 (whose
 * lo/hi are free) and range-check in the handler instead. */
#define CLI_A_NUM	1	/* decimal number, range in lo/hi */
#define CLI_A_WORD	2	/* any single word */
#define CLI_A_IP	3	/* dotted quad */
#define CLI_A_IFACE	4	/* ethernet 1/N | eN/M | line rest */
#define CLI_A_LINE	5	/* rest of the line, verbatim */
#define CLI_A_NUM32	6	/* decimal up to 9 digits; the handler range-checks */
#define CLI_A_HEX	7	/* hexadecimal up to 8 digits, optional 0x; lo/hi free */
#define CLI_A_IFLIST	8	/* 1/1-4,1/7: ports as a mask (bit N) */

#define CLI_MAX_ARGS	4

struct cli_node {
	__code const char *word;	/* 0 for an argument placeholder */
	uint8_t arg;			/* CLI_A_* when word == 0 */
	uint8_t flags;
	uint16_t lo, hi;		/* CLI_A_NUM range */
	__code const struct cli_node * __code const *children;
	uint8_t action;			/* CLI_ACT_*, 0 = not executable here */
	__code const char *help;
};

struct cli_state_t {
	/* ---- per session: swapped by cli_use(), keep these first ---- */
	uint8_t mode;
	uint8_t await;		/* interactive sub-prompt, AWAIT_* */
	uint8_t ctx_if;		/* user-facing port number for MODE_IF */
	uint8_t ctx_lport;	/* logical (driver) port for MODE_IF */
	uint16_t ctx_vlan;	/* vlan id for MODE_VLAN and MODE_SVI */
	uint8_t ctx_line;	/* 0 = console, 1 = vty */
	uint8_t ctx_po;		/* port-channel 1-4 for MODE_PO */
	uint16_t ctx_range;	/* MODE_IF on a range: user ports as a mask (bit N), else 0 */
	uint8_t plen;		/* printed prompt width, aligns the '^' marker */
	/* ---- per line: a line always runs to completion ---- */
	uint8_t no;		/* current line carries a `no` prefix */
	uint8_t nargs;
	uint32_t args[CLI_MAX_ARGS];
	/* raw offset of each arg token in the line, for string args */
	uint8_t argoff[CLI_MAX_ARGS];
	uint8_t argerr;		/* offset of the arg that failed validation */
	uint16_t acc;		/* OR of ->lo of the CLI_F_ACC literals matched */
	uint16_t lo;		/* ->lo of the matched node, for cli_act() */
	__xdata char *line;	/* the line being executed, for cli_act() */
};

/* Sessions: the serial console and the telnet vty each keep their own
 * mode and submode context. Every entry point (line editor, telnet
 * server, main loop) selects its session first; switching saves the
 * per-session part of `cli` and loads the other one. */
#define CLI_CONSOLE	0
#define CLI_VTY		1
void cli_use(uint8_t who) __banked;

#define CLI_AWAIT_NONE		0
#define CLI_AWAIT_ENABLE_PW	1

extern __xdata struct cli_state_t cli;

void cli_init(void) __banked;
/* Execute one line; prints results and errors itself. */
void cli_exec_line(__xdata char *line) __banked;
/* Print the prompt for the current mode (with hostname). */
void cli_prompt(void) __banked;
/* `?` pressed: list candidates for the (partial) line. */
void cli_help(__xdata char *line) __banked;
/* Tab pressed: returns completion suffix chars appended to line, 0 if none.
 * line must have room for the completion. */
uint8_t cli_complete(__xdata char *line, uint8_t maxlen) __banked;
/* True while the CLI expects a password on the next line (no echo). */
uint8_t cli_hidden_input(void) __banked;

/* Boot-time startup-config replay: lines run in global config mode and
 * VLAN pushes are deferred to the end. A line that fails is reported with
 * the line itself and skipped. */
void cli_replay_begin(void) __banked;
void cli_replay_line(__xdata char *line) __banked;
void cli_replay_end(void) __banked;
/* set while the startup config replays: handlers skip advisory warnings */
extern __xdata uint8_t cli_replaying;

#endif
