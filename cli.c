/*
 * Modal CLI engine. See cli.h for the model.
 *
 * The command tree lives in code space; matching walks one token at a
 * time with unique-prefix abbreviation. Literal tokens win over
 * argument placeholders. EXEC commands are reachable from config modes
 * without `do` (mode root first, EXEC root second), and a line whose
 * first token no tree claims falls back to the legacy flat parser so
 * unported commands keep working during the migration.
 *
 * There is ONE cli state shared by the serial console and telnet: on
 * this hardware the sessions share the underlying command machinery,
 * so a mode change on one is visible on the other. Documented
 * limitation until the legacy parser is gone.
 *
 * Handlers are action ids dispatched from a switch, not function
 * pointers: banked function pointers are fragile with SDCC, ids are
 * free.
 */
#include "rtl837x_common.h"
#include "cmd_parser.h"
#include "cli.h"

#pragma codeseg BANK1
#pragma constseg BANK1

extern __xdata char hostname[24];
void reset_chip(void);

__xdata struct cli_state_t cli;
/* Printable prompt width (without the leading newline), for aligning
 * the '^' error marker under the echoed line. */
__xdata uint8_t cli_plen;

/* ---- tokenizer state ---- */
#define CLI_MAX_TOKS 10
static __xdata uint8_t tok_off[CLI_MAX_TOKS];
static __xdata uint8_t tok_len[CLI_MAX_TOKS];
static __xdata uint8_t ntok;
static __xdata uint8_t trailing_space;
static __xdata char *cli_line;

/* ---- walk result ---- */
static __xdata uint8_t w_status;
#define W_OK		0	/* stopped at w_node with all tokens eaten */
#define W_NOMATCH0	1	/* first token matched nothing */
#define W_INVALID	2	/* some later token matched nothing */
#define W_AMBIG		3
static __xdata uint8_t w_badtok;	/* token index of the failure */
static __code const struct cli_node * __xdata w_node;
static __code const struct cli_node * __code const * __xdata w_children;

/* ---------------- actions ---------------- */
#define ACT_NONE	0
#define ACT_ENABLE	1
#define ACT_DISABLE	2
#define ACT_CONF_T	3
#define ACT_EXIT	4
#define ACT_END		5
#define ACT_SHOW_VER	6
#define ACT_WRITE	7
#define ACT_RELOAD	8
#define ACT_IF		9
#define ACT_VLAN	10
#define ACT_LEGACY	11	/* re-run the whole line in the legacy parser */

/* ---------------- command tree ---------------- */

/* leaf with no children */
#define NO_CHILDREN 0

static __code const struct cli_node n_conf_terminal = {
	"terminal", 0, CLI_F_PRIV, 0, 0, NO_CHILDREN, ACT_CONF_T,
	"Configure from the terminal"
};
static __code const struct cli_node * __code const ch_configure[] = {
	&n_conf_terminal, 0
};

static __code const struct cli_node n_show_version = {
	"version", 0, 0, 0, 0, NO_CHILDREN, ACT_SHOW_VER,
	"System software and hardware status"
};
static __code const struct cli_node * __code const ch_show[] = {
	&n_show_version, 0
};

static __code const struct cli_node n_write_memory = {
	"memory", 0, CLI_F_PRIV, 0, 0, NO_CHILDREN, ACT_WRITE,
	"Write the running configuration to flash"
};
static __code const struct cli_node * __code const ch_write[] = {
	&n_write_memory, 0
};

/* copy running-config startup-config, plus the legacy TFTP forms
 * (copy tftp ... / copy config tftp ...) passed through verbatim */
static __code const struct cli_node n_copy_run_start = {
	"startup-config", 0, CLI_F_PRIV, 0, 0, NO_CHILDREN, ACT_WRITE,
	"Save to the startup configuration"
};
static __code const struct cli_node * __code const ch_copy_run[] = {
	&n_copy_run_start, 0
};
static __code const struct cli_node n_copy_running = {
	"running-config", 0, CLI_F_PRIV, 0, 0, ch_copy_run, ACT_NONE,
	"Copy from the running configuration"
};
static __code const struct cli_node n_arg_rest_legacy = {
	0, CLI_A_LINE, CLI_F_PRIV, 0, 0, NO_CHILDREN, ACT_LEGACY,
	"flash|config <server-ip> <filename>"
};
static __code const struct cli_node * __code const ch_copy_tftp[] = {
	&n_arg_rest_legacy, 0
};
static __code const struct cli_node n_copy_tftp = {
	"tftp", 0, CLI_F_PRIV, 0, 0, ch_copy_tftp, ACT_NONE,
	"Download from a TFTP server"
};
static __code const struct cli_node n_copy_config = {
	"config", 0, CLI_F_PRIV, 0, 0, ch_copy_tftp, ACT_NONE,
	"Upload the startup config (copy config tftp <ip> <file>)"
};
static __code const struct cli_node * __code const ch_copy[] = {
	&n_copy_running, &n_copy_tftp, &n_copy_config, 0
};

static __code const struct cli_node n_enable = {
	"enable", 0, 0, 0, 0, NO_CHILDREN, ACT_ENABLE,
	"Turn on privileged commands"
};
static __code const struct cli_node n_disable = {
	"disable", 0, CLI_F_PRIV, 0, 0, NO_CHILDREN, ACT_DISABLE,
	"Turn off privileged commands"
};
static __code const struct cli_node n_configure = {
	"configure", 0, CLI_F_PRIV, 0, 0, ch_configure, ACT_NONE,
	"Enter configuration mode"
};
static __code const struct cli_node n_show = {
	"show", 0, 0, 0, 0, ch_show, ACT_NONE,
	"Show running system information"
};
static __code const struct cli_node n_write = {
	"write", 0, CLI_F_PRIV, 0, 0, ch_write, ACT_WRITE,
	"Write the running configuration to flash"
};
static __code const struct cli_node n_copy = {
	"copy", 0, CLI_F_PRIV, 0, 0, ch_copy, ACT_NONE,
	"Copy configuration or image data"
};
static __code const struct cli_node n_reload = {
	"reload", 0, CLI_F_PRIV, 0, 0, NO_CHILDREN, ACT_RELOAD,
	"Halt and perform a cold restart"
};
static __code const struct cli_node n_exit_exec = {
	"exit", 0, 0, 0, 0, NO_CHILDREN, ACT_EXIT,
	"Exit the current mode"
};

static __code const struct cli_node * __code const cli_root_exec[] = {
	&n_configure, &n_copy, &n_disable, &n_enable, &n_exit_exec,
	&n_reload, &n_show, &n_write, 0
};

/* config mode */
static __code const struct cli_node n_arg_ifnum = {
	0, CLI_A_IFACE, 0, 0, 0, NO_CHILDREN, ACT_IF,
	"Interface number (e.g. 1/5)"
};
static __code const struct cli_node * __code const ch_iface[] = {
	&n_arg_ifnum, 0
};
static __code const struct cli_node n_if_ethernet = {
	"ethernet", 0, 0, 0, 0, ch_iface, ACT_NONE,
	"Ethernet interface"
};
static __code const struct cli_node * __code const ch_interface[] = {
	&n_if_ethernet, &n_arg_ifnum, 0
};
static __code const struct cli_node n_interface = {
	"interface", 0, 0, 0, 0, ch_interface, ACT_NONE,
	"Select an interface to configure"
};

static __code const struct cli_node n_arg_vlanid = {
	0, CLI_A_NUM, 0, 1, 4094, NO_CHILDREN, ACT_VLAN,
	"VLAN id"
};
static __code const struct cli_node * __code const ch_vlan[] = {
	&n_arg_vlanid, 0
};
static __code const struct cli_node n_vlan = {
	"vlan", 0, 0, 0, 0, ch_vlan, ACT_NONE,
	"Add, delete or modify a VLAN"
};

static __code const struct cli_node n_exit_cfg = {
	"exit", 0, 0, 0, 0, NO_CHILDREN, ACT_EXIT,
	"Exit the current mode"
};
static __code const struct cli_node n_end = {
	"end", 0, 0, 0, 0, NO_CHILDREN, ACT_END,
	"Exit to privileged EXEC mode"
};

static __code const struct cli_node * __code const cli_root_config[] = {
	&n_end, &n_exit_cfg, &n_interface, &n_vlan, 0
};

static __code const struct cli_node * __code const cli_root_sub[] = {
	&n_end, &n_exit_cfg, 0
};


static __code const struct cli_node * __code const *root_for_mode(uint8_t mode)
{
	switch (mode) {
	case CLI_MODE_CONFIG:
		return cli_root_config;
	case CLI_MODE_IF:
	case CLI_MODE_VLAN:
	case CLI_MODE_LINE:
		return cli_root_sub;
	default:
		return cli_root_exec;
	}
}


/* ---------------- helpers ---------------- */

static char lc(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c | 0x20;
	return c;
}


static void cli_tokenize(__xdata char *line)
{
	__xdata uint8_t i = 0;

	cli_line = line;
	ntok = 0;
	trailing_space = 1;
	while (line[i]) {
		if (line[i] == ' ') {
			i++;
			continue;
		}
		if (ntok >= CLI_MAX_TOKS)
			break;
		tok_off[ntok] = i;
		while (line[i] && line[i] != ' ')
			i++;
		tok_len[ntok] = i - tok_off[ntok];
		ntok++;
		trailing_space = (line[i] == ' ') ? 1 : 0;
	}
	if (!ntok)
		trailing_space = 1;
}


/* Is token t a prefix of the code-space word? 2 = exact match. */
static __xdata char *tm_p;
static __xdata uint8_t tm_n;

static uint8_t tok_matches(uint8_t t, __code const char *word)
{
	tm_p = cli_line + tok_off[t];
	tm_n = tok_len[t];

	while (tm_n) {
		if (!*word || lc(*tm_p) != *word)
			return 0;
		tm_p++;
		word++;
		tm_n--;
	}
	return *word ? 1 : 2;
}


static uint8_t node_visible(__code const struct cli_node *n)
{
	if ((n->flags & CLI_F_PRIV) && cli.mode == CLI_MODE_EXEC)
		return 0;
	if ((n->flags & CLI_F_NO_ONLY) && !cli.no)
		return 0;
	return 1;
}


/* Validate token t against an argument node; store the value. Returns 0
 * on mismatch. */
static uint8_t arg_accept(uint8_t t, __code const struct cli_node *n)
{
	__xdata char *p = cli_line + tok_off[t];
	__xdata uint8_t len = tok_len[t];
	__xdata uint32_t v = 0;
	__xdata uint8_t i = 0;

	if (cli.nargs >= CLI_MAX_ARGS)
		return 0;

	switch (n->arg) {
	case CLI_A_NUM:
		if (!len || len > 5)
			return 0;
		for (i = 0; i < len; i++) {
			if (p[i] < '0' || p[i] > '9')
				return 0;
			v = v * 10 + (p[i] - '0');
		}
		if (v < n->lo || v > n->hi)
			return 0;
		break;
	case CLI_A_IP:
	{
		static __xdata uint8_t dots, digits;
		static __xdata uint16_t oct;
		dots = 0; digits = 0; oct = 0;
		v = 0;
		for (i = 0; i < len; i++) {
			if (p[i] == '.') {
				if (!digits || dots == 3)
					return 0;
				v = (v << 8) | oct;
				oct = 0;
				digits = 0;
				dots++;
			} else if (p[i] >= '0' && p[i] <= '9') {
				oct = oct * 10 + (p[i] - '0');
				if (oct > 255 || ++digits > 3)
					return 0;
			} else {
				return 0;
			}
		}
		if (dots != 3 || !digits)
			return 0;
		v = (v << 8) | oct;
		break;
	}
	case CLI_A_IFACE:
	{
		/* [ethernet-prefix]N or [ethernet-prefix]S/N; the port is N */
		static __code const char * __xdata w;
		static __xdata uint16_t num;
		static __xdata uint8_t have;
		w = "ethernet"; num = 0; have = 0;
		i = 0;
		while (i < len && ((p[i] >= 'a' && p[i] <= 'z') || (p[i] >= 'A' && p[i] <= 'Z'))) {
			if (!*w || lc(p[i]) != *w)
				return 0;
			w++;
			i++;
		}
		for (; i < len; i++) {
			if (p[i] == '/') {
				if (!have)
					return 0;
				num = 0;
				have = 0;
			} else if (p[i] >= '0' && p[i] <= '9') {
				num = num * 10 + (p[i] - '0');
				have = 1;
				if (num > 99)
					return 0;
			} else {
				return 0;
			}
		}
		if (!have || num < 1 || num > 9)
			return 0;
		v = num;
		break;
	}
	case CLI_A_WORD:
		if (!len)
			return 0;
		break;
	case CLI_A_LINE:
		break;
	default:
		return 0;
	}

	cli.args[cli.nargs] = v;
	cli.argoff[cli.nargs] = tok_off[t];
	cli.nargs++;
	return 1;
}


/* Walk tokens [0, upto) through the given root. */
static void cli_walk(__code const struct cli_node * __code const *root, uint8_t upto)
{
	__xdata uint8_t t;

	w_children = root;
	w_node = 0;
	w_status = W_OK;
	w_badtok = 0;
	cli.nargs = 0;

	for (t = 0; t < upto; t++) {
		__code const struct cli_node * __xdata exact = 0;
		__code const struct cli_node * __xdata pref = 0;
		__xdata uint8_t npref = 0;
		__code const struct cli_node * __code const * __xdata c;
		__code const struct cli_node * __xdata sel = 0;

		if (!w_children) {
			w_status = W_INVALID;
			w_badtok = t;
			return;
		}
		for (c = w_children; *c; c++) {
			if (!node_visible(*c))
				continue;
			if ((*c)->word) {
				static __xdata uint8_t m;
				m = tok_matches(t, (*c)->word);
				if (m == 2)
					exact = *c;
				else if (m == 1) {
					pref = *c;
					npref++;
				}
			}
		}
		if (exact)
			sel = exact;
		else if (npref == 1)
			sel = pref;
		else if (npref > 1) {
			w_status = W_AMBIG;
			w_badtok = t;
			return;
		}
		if (!sel) {
			/* argument placeholders, in table order */
			for (c = w_children; *c; c++) {
				if (!node_visible(*c) || (*c)->word)
					continue;
				if (arg_accept(t, *c)) {
					sel = *c;
					break;
				}
			}
		}
		if (!sel) {
			w_status = (t == 0) ? W_NOMATCH0 : W_INVALID;
			w_badtok = t;
			return;
		}
		w_node = sel;
		if (!sel->word && sel->arg == CLI_A_LINE)
			return;	/* swallows the rest of the line */
		w_children = sel->children;
	}
}


/* ---------------- output helpers ---------------- */

static void cli_spaces(uint8_t n)
{
	while (n--)
		write_char(' ');
}


static void cli_marker_error(void)
{
	cli_spaces(cli_plen + tok_off[w_badtok]);
	print_string("^\n% Invalid input detected at '^' marker.\n\n");
}


static void print_placeholder(__code const struct cli_node *n)
{
	switch (n->arg) {
	case CLI_A_NUM:
		write_char('<');
		itoa_short(n->lo);
		write_char('-');
		itoa_short(n->hi);
		write_char('>');
		break;
	case CLI_A_IP:
		print_string("A.B.C.D");
		break;
	case CLI_A_IFACE:
		print_string("<1-9> or S/N");
		break;
	case CLI_A_WORD:
		print_string("WORD");
		break;
	case CLI_A_LINE:
		print_string("LINE");
		break;
	}
}


static __xdata uint8_t csl_n;

static uint8_t code_strlen(__code const char *s)
{
	csl_n = 0;
	while (*s++)
		csl_n++;
	return csl_n;
}


/* When help lists the EXEC root behind a config-mode root, skip words
 * the mode root already listed (exit/end live in both). */
static __code const struct cli_node * __code const * __xdata dedupe_root;

static uint8_t code_streq(__code const char *a, __code const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static uint8_t word_in_dedupe_root(__code const char *w)
{
	__code const struct cli_node * __code const * __xdata c;

	if (!dedupe_root)
		return 0;
	for (c = dedupe_root; *c; c++) {
		if ((*c)->word && code_streq((*c)->word, w))
			return 1;
	}
	return 0;
}


static void cli_list_candidates(uint8_t partial_tok)
{
	__code const struct cli_node * __code const * __xdata c;
	__xdata uint8_t width = 0;
	__xdata uint8_t any = 0;

	if (!w_children) {
		if (w_node && w_node->action)
			print_string("  <cr>\n");
		return;
	}
	/* column width from the longest literal */
	for (c = w_children; *c; c++) {
		if (!node_visible(*c))
			continue;
		if ((*c)->word) {
			static __xdata uint8_t n;
			n = code_strlen((*c)->word);
			if (n > width)
				width = n;
		}
	}
	if (width < 14)
		width = 14;
	for (c = w_children; *c; c++) {
		if (!node_visible(*c))
			continue;
		if (partial_tok != 0xff && (*c)->word
		    && !tok_matches(partial_tok, (*c)->word))
			continue;
		if ((*c)->word && word_in_dedupe_root((*c)->word))
			continue;
		write_char(' ');
		write_char(' ');
		if ((*c)->word) {
			static __xdata uint8_t n;
			n = code_strlen((*c)->word);
			print_string((*c)->word);
			cli_spaces(width - n + 2);
		} else {
			print_placeholder(*c);
			print_string("  ");
		}
		if ((*c)->help)
			print_string((*c)->help);
		write_char('\n');
		any = 1;
	}
	if (w_node && w_node->action && partial_tok == 0xff)
		print_string("  <cr>\n");
	if (!any && partial_tok != 0xff)
		print_string("% Unrecognized command\n");
}


/* ---------------- actions ---------------- */

static void cli_dispatch(uint8_t action)
{
	switch (action) {
	case ACT_ENABLE:
		if (cli.mode == CLI_MODE_EXEC)
			cli.mode = CLI_MODE_PRIV;
		break;
	case ACT_DISABLE:
		cli.mode = CLI_MODE_EXEC;
		break;
	case ACT_CONF_T:
		cli.mode = CLI_MODE_CONFIG;
		print_string("Enter configuration commands, one per line. End with 'end'.\n");
		break;
	case ACT_EXIT:
		switch (cli.mode) {
		case CLI_MODE_CONFIG:
			cli.mode = CLI_MODE_PRIV;
			break;
		case CLI_MODE_IF:
		case CLI_MODE_VLAN:
		case CLI_MODE_LINE:
			cli.mode = CLI_MODE_CONFIG;
			break;
		/* EXEC/PRIV: telnet intercepts `exit` itself; nothing to do
		 * on the serial console. */
		}
		break;
	case ACT_END:
		if (cli.mode >= CLI_MODE_CONFIG)
			cli.mode = CLI_MODE_PRIV;
		break;
	case ACT_SHOW_VER:
		print_sw_version();
		break;
	case ACT_WRITE:
		print_string("Building configuration...\n");
		cmd_save_config();
		break;
	case ACT_RELOAD:
		print_string("\nRELOAD\n\n");
		reset_chip();
		break;
	case ACT_IF:
		cli.ctx_if = cli.args[0];
		cli.mode = CLI_MODE_IF;
		break;
	case ACT_VLAN:
		cli.ctx_vlan = cli.args[0];
		cli.mode = CLI_MODE_VLAN;
		break;
	case ACT_LEGACY:
		execute_commands((__xdata uint8_t *)cli_line);
		break;
	}
}


/* ---------------- public entry points ---------------- */

void cli_init(void) __banked
{
	cli.mode = CLI_MODE_EXEC;
	cli.await = CLI_AWAIT_NONE;
	cli.no = 0;
}


uint8_t cli_hidden_input(void) __banked
{
	return cli.await != CLI_AWAIT_NONE;
}


/* Match the tokenized line against the mode root, then the EXEC root
 * (commands-anywhere), for tokens [start, upto). Returns 1 when some
 * root accepted the first token. */
static uint8_t cli_walk_roots(uint8_t upto)
{
	cli_walk(root_for_mode(cli.mode), upto);
	if (w_status == W_NOMATCH0 && cli.mode >= CLI_MODE_CONFIG && !cli.no) {
		cli_walk(cli_root_exec, upto);
	}
	return w_status != W_NOMATCH0;
}


void cli_exec_line(__xdata char *line) __banked
{
	cli_tokenize(line);
	cli.no = 0;

	if (!ntok)
		return;

	/* `no` prefix in configuration modes */
	if (cli.mode >= CLI_MODE_CONFIG && tok_matches(0, "no")) {
		__xdata uint8_t i;
		cli.no = 1;
		for (i = 1; i < ntok; i++) {
			tok_off[i - 1] = tok_off[i];
			tok_len[i - 1] = tok_len[i];
		}
		ntok--;
		if (!ntok) {
			print_string("% Incomplete command.\n\n");
			return;
		}
	}

	if (!cli_walk_roots(ntok)) {
		/* Nothing in the tree claims this line: legacy parser */
		execute_commands((__xdata uint8_t *)line);
		return;
	}

	switch (w_status) {
	case W_AMBIG:
		print_string("% Ambiguous command:  \"");
		print_string_x((__xdata char *)line);
		print_string("\"\n\n");
		return;
	case W_INVALID:
		cli_marker_error();
		return;
	}

	if (!w_node || !w_node->action) {
		print_string("% Incomplete command.\n\n");
		return;
	}
	if (cli.no && !(w_node->flags & (CLI_F_NO_OK | CLI_F_NO_ONLY))) {
		cli_marker_error();
		return;
	}
	cli_dispatch(w_node->action);
}


/* No leading newline: callers position the cursor (fresh line after a
 * command, or an in-place redraw from the line editor). */
void cli_prompt(void) __banked
{
	__xdata char *h = hostname;

	cli_plen = 0;
	while (*h) {
		write_char(*h++);
		cli_plen++;
	}
	switch (cli.mode) {
	case CLI_MODE_EXEC:
		print_string("> ");
		cli_plen += 2;
		return;
	case CLI_MODE_CONFIG:
		print_string("(config)");
		cli_plen += 8;
		break;
	case CLI_MODE_IF:
		print_string("(config-if)");
		cli_plen += 11;
		break;
	case CLI_MODE_VLAN:
		print_string("(config-vlan)");
		cli_plen += 13;
		break;
	case CLI_MODE_LINE:
		print_string("(config-line)");
		cli_plen += 13;
		break;
	}
	print_string("# ");
	cli_plen += 2;
}


void cli_help(__xdata char *line) __banked
{
	cli_tokenize(line);
	cli.no = 0;

	if (!ntok || (ntok == 1 && !trailing_space)) {
		/* listing the root: match nothing, list all (filtered by a
		 * partial first token if present) */
		w_children = root_for_mode(cli.mode);
		w_node = 0;
		cli.nargs = 0;
		dedupe_root = 0;
		cli_list_candidates(ntok ? 0 : 0xff);
		if (cli.mode >= CLI_MODE_CONFIG) {
			dedupe_root = root_for_mode(cli.mode);
			w_children = cli_root_exec;
			cli_list_candidates(ntok ? 0 : 0xff);
			dedupe_root = 0;
		}
		return;
	}

	if (trailing_space) {
		/* all tokens complete: list children of the match */
		if (!cli_walk_roots(ntok) || w_status != W_OK) {
			print_string("% Unrecognized command\n");
			return;
		}
		cli_list_candidates(0xff);
	} else {
		/* last token partial: filter its candidates */
		if (!cli_walk_roots(ntok - 1) || w_status != W_OK) {
			print_string("% Unrecognized command\n");
			return;
		}
		cli_list_candidates(ntok - 1);
	}
}


uint8_t cli_complete(__xdata char *line, uint8_t maxlen) __banked
{
	__code const struct cli_node * __code const * __xdata c;
	__code const struct cli_node * __xdata cand = 0;
	__xdata uint8_t ncand = 0;
	__xdata uint8_t len, added = 0;
	static __code const char * __xdata w;

	cli_tokenize(line);
	cli.no = 0;
	if (!ntok || trailing_space)
		return 0;

	if (ntok == 1) {
		w_children = root_for_mode(cli.mode);
	} else {
		if (!cli_walk_roots(ntok - 1) || w_status != W_OK || !w_children)
			return 0;
	}
	for (c = w_children; *c; c++) {
		if (!node_visible(*c) || !(*c)->word)
			continue;
		if (tok_matches(ntok - 1, (*c)->word)) {
			cand = *c;
			ncand++;
		}
	}
	/* the first token in a config mode may also complete from EXEC */
	if (ntok == 1 && cli.mode >= CLI_MODE_CONFIG) {
		for (c = cli_root_exec; *c; c++) {
			if (!node_visible(*c) || !(*c)->word)
				continue;
			if (tok_matches(0, (*c)->word)) {
				cand = *c;
				ncand++;
			}
		}
	}
	if (ncand != 1)
		return 0;

	len = tok_len[ntok - 1];
	w = cand->word + len;
	while (*w && (uint8_t)(tok_off[ntok - 1] + len + added) < maxlen - 2) {
		line[tok_off[ntok - 1] + len + added] = *w++;
		added++;
	}
	line[tok_off[ntok - 1] + len + added] = ' ';
	added++;
	line[tok_off[ntok - 1] + len + added] = 0;
	return added;
}
