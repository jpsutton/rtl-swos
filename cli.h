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
 *  - a line no tree node claims falls back to the legacy flat parser,
 *    so unported commands keep working during the migration
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

/* Node flags */
#define CLI_F_PRIV	0x01	/* hidden and refused in user EXEC */
#define CLI_F_NO_OK	0x02	/* usable under `no` */
#define CLI_F_NO_ONLY	0x04	/* only usable under `no` */
#define CLI_F_NO_EXEC	0x08	/* executable here only under `no`; plain form is incomplete */

/* Argument placeholder types (a node with word == 0 is an argument) */
#define CLI_A_NONE	0
#define CLI_A_NUM	1	/* decimal number, range in lo/hi */
#define CLI_A_WORD	2	/* any single word */
#define CLI_A_IP	3	/* dotted quad */
#define CLI_A_IFACE	4	/* ethernet 1/N | eN/M | line rest */
#define CLI_A_LINE	5	/* rest of the line, verbatim */

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
	uint8_t mode;
	uint8_t no;		/* current line carries a `no` prefix */
	uint8_t await;		/* interactive sub-prompt, AWAIT_* */
	uint8_t ctx_if;		/* user-facing port number for MODE_IF */
	uint8_t ctx_lport;	/* logical (driver) port for MODE_IF */
	uint16_t ctx_vlan;	/* vlan id for MODE_VLAN and MODE_SVI */
	uint8_t ctx_line;	/* 0 = console, 1 = vty */
	uint8_t nargs;
	uint32_t args[CLI_MAX_ARGS];
	/* raw offset of each arg token in the line, for string args */
	uint8_t argoff[CLI_MAX_ARGS];
	uint8_t argerr;		/* offset of the arg that failed validation */
};

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

/* Boot-time startup-config replay: lines run in global config mode; a
 * line the modal parser cannot run goes to the legacy parser instead,
 * so a config written in the old flat syntax still boots. VLAN pushes
 * are deferred to the end. */
void cli_replay_begin(void) __banked;
void cli_replay_line(__xdata char *line) __banked;
void cli_replay_end(void) __banked;

#endif
