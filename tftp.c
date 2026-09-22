/*
 * Minimal TFTP client (RFC 1350, octet mode) for firmware and
 * startup-config transfer, driving the same staged-update path the web
 * upload used: a firmware image lands in the staging area at
 * FIRMWARE_UPLOAD_START, is CRC-verified, and the boot-time updater
 * copies it to the start of flash after reset.
 *
 * The transfer runs asynchronously from the uIP UDP callback, so
 * progress output goes to the serial console (and syslog); a telnet
 * session only sees the command's acceptance message.
 *
 * uIP addresses outgoing UDP packets from conn->rport, and the TFTP
 * server answers a request from a fresh transfer TID, not from port 69.
 * The request is therefore sent with rport = 69, rport is widened to 0
 * (wildcard) on the following poll tick, and the connection is locked
 * to the server's TID when its first reply arrives. A server reply
 * landing inside that one-tick window is dropped and covered by the
 * standard TFTP retransmission of either side.
 */
#include <8051.h>
#include "rtl837x_common.h"
#include "rtl837x_flash.h"
#include "cmd_parser.h"
#include "uip/uip.h"
#include "tftp.h"
#include "runcfg.h"

#pragma codeseg BANK3
#pragma constseg BANK3

extern volatile __xdata uint32_t ticks;
extern __xdata uint8_t ip[4];		/* filled by parse_ip() */
extern __xdata uint32_t flash_size;
extern __xdata uint16_t crc_value;
extern __xdata uint8_t flash_buf[FLASH_BUF_SIZE];
extern __xdata struct flash_region_t flash_region;
void crc16_bank1(__xdata uint8_t *v) __naked;
void reset_chip(void);
void delay(uint16_t t);

#define UDPBUF ((__xdata struct uip_udpip_hdr *)&uip_buf[UIP_LLH_LEN])

/* Staging area size == everything from FIRMWARE_UPLOAD_START to the end
 * of the first flash megabyte; the image must fill it exactly, since the
 * boot-time CRC runs over the whole area. */
#define TFTP_FW_SIZE ((uint32_t)FIRMWARE_UPLOAD_START)

#define TFTP_BLOCK 512

/* Protocol opcodes */
#define OP_RRQ   1
#define OP_WRQ   2
#define OP_DATA  3
#define OP_ACK   4
#define OP_ERROR 5

/* States */
#define T_OFF 0
#define T_REQ 1	/* RRQ/WRQ queued or sent, awaiting first reply */
#define T_RX  2
#define T_TX  3
#define T_FIN 4	/* final ACK queued; verify and reset on the next poll */

#define TFTP_TIMEOUT_TICKS (2 * SYS_TICK_HZ)
#define TFTP_MAX_RETRIES 8

static __xdata struct {
	uint8_t state;
	uint8_t op;
	uint8_t retries;
	uint8_t req_sent;	/* widen rport to wildcard on next poll */
	uint8_t last_len_short;	/* TX: final (short) block has been sent */
	uint16_t blk;		/* RX: last block ACKed; TX: last block sent */
	uint16_t txlen;		/* TX: total config length */
	uint32_t addr;		/* RX: next flash write address */
	uint32_t tick_last;
	__xdata struct uip_udp_conn *conn;
	char fname[TFTP_FNAME_SIZE];
} tftp;

static __xdata uint32_t tick_snap;
static __xdata uip_ipaddr_t tftp_server;

/* Outcome of the last transfer, for show tftp: the transfer runs from
 * the UDP callback and reports to the serial console only */
static __code const char * __xdata tftp_last;
static __xdata uint8_t tftp_last_op;
static __xdata uint16_t tftp_last_blk;


static uint32_t ticks_now(void)
{
	EA = 0;
	tick_snap = ticks;
	EA = 1;
	return tick_snap;
}


void tftp_init(void) __banked
{
	tftp.state = T_OFF;
	tftp.conn = 0;
	tftp_last = 0;
	tftp_last_op = 0;
	tftp_last_blk = 0;
}


void tftp_show(void) __banked
{
	static __code const char * __code const opname[] = {
		"-", "firmware download", "config download", "config upload"
	};

	if (tftp_busy()) {
		print_string("Transfer in progress: ");
		print_string(opname[tftp.op & 3]);
		print_string(", file ");
		print_string_x(tftp.fname);
		print_string(", block ");
		itoa_short(tftp.blk);
		print_string(tftp.state == T_REQ ? " (waiting for the server)\n" : "\n");
		return;
	}
	if (!tftp_last_op) {
		print_string("No transfer since boot\n");
		return;
	}
	print_string("Last transfer: ");
	print_string(opname[tftp_last_op & 3]);
	print_string(tftp_last ? ", failed: " : ", completed\n");
	if (tftp_last) {
		print_string(tftp_last);
		print_string(" (at block ");
		itoa_short(tftp_last_blk);
		print_string(")\n");
	}
}


/* A real transfer always owns a UDP connection bound to the client
 * port. Any other non-idle state is stale (it was once found set at
 * boot for reasons still under investigation) and must never lock out
 * firmware updates, so it is cleared here. */
uint8_t tftp_busy(void) __banked
{
	static __xdata uint8_t i;

	if (tftp.state == T_OFF)
		return 0;
	for (i = 0; i < UIP_UDP_CONNS; i++) {
		if (tftp.conn == &uip_udp_conns[i]
		    && uip_udp_conns[i].lport == HTONS(TFTP_CLIENT_PORT))
			return 1;
	}
	tftp.state = T_OFF;
	tftp.conn = 0;
	return 0;
}


static void tftp_teardown(void)
{
	if (tftp.conn) {
		uip_udp_remove(tftp.conn);
		tftp.conn = 0;
	}
	tftp.state = T_OFF;
}


static void tftp_abort(__code const char *msg)
{
	tftp_last = msg;
	tftp_last_blk = tftp.blk;
	print_string("\nTFTP failed: ");
	print_string(msg);
	write_char('\n');
	if (tftp.op == TFTP_OP_GET_FW && tftp.addr > FIRMWARE_UPLOAD_START) {
		/* Neutralize the partial image so the boot-time updater does
		 * not find its magic bytes. */
		flash_region.addr = FIRMWARE_UPLOAD_START;
		flash_sector_erase();
	}
	tftp_teardown();
}


void tftp_begin(uint8_t op, __xdata const char *fname) __banked
{
	__xdata char *d = tftp.fname;
	uint8_t n = 0;

	if (tftp_busy()) {
		print_string("TFTP transfer already in progress\n");
		return;
	}
	if (op == TFTP_OP_GET_FW && flash_size < FIRMWARE_UPLOAD_START * 2) {
		print_string("Flash too small for staged firmware update\n");
		return;
	}

	while (*fname && *fname != ' ' && n < TFTP_FNAME_SIZE - 1) {
		*d++ = *fname++;
		n++;
	}
	*d = 0;
	if (!n) {
		print_string("Missing filename\n");
		return;
	}

	uip_ipaddr(&tftp_server, ip[0], ip[1], ip[2], ip[3]);
	tftp.conn = uip_udp_new(&tftp_server, HTONS(TFTP_SERVER_PORT));
	if (!tftp.conn) {
		print_string("No free UDP socket\n");
		return;
	}
	uip_udp_bind(tftp.conn, HTONS(TFTP_CLIENT_PORT));

	tftp.op = op;
	tftp_last_op = op;
	tftp_last = 0;
	tftp.state = T_REQ;
	tftp.retries = 0;
	tftp.req_sent = 0;
	tftp.last_len_short = 0;
	tftp.blk = 0;
	tftp.addr = FIRMWARE_UPLOAD_START;
	tftp.txlen = 0;
	tftp.tick_last = ticks_now();

	if (op == TFTP_OP_PUT_CONFIG) {
		/* Length of the stored config text, up to its terminator */
		__xdata uint32_t pos = CONFIG_START;
		__xdata uint16_t i;
		while (pos < CONFIG_START + CONFIG_LEN) {
			flash_region.addr = pos;
			flash_region.len = FLASH_BUF_SIZE;
			flash_read_bulk(flash_buf);
			for (i = 0; i < FLASH_BUF_SIZE; i++) {
				if (flash_buf[i] == 0 || flash_buf[i] == 0xff)
					goto len_done;
				tftp.txlen++;
			}
			pos += FLASH_BUF_SIZE;
		}
len_done:
		;
	}

	print_string("TFTP transfer started, watch the serial console\n");
}


/* Build RRQ/WRQ into uip_appdata, return its length */
static uint16_t tftp_req_build(void)
{
	__xdata uint8_t *p = uip_appdata;
	__xdata char *f = tftp.fname;

	*p++ = 0;
	*p++ = (tftp.op == TFTP_OP_PUT_CONFIG) ? OP_WRQ : OP_RRQ;
	while (*f)
		*p++ = *f++;
	*p++ = 0;
	p += strtox(p, "octet");
	p++;	/* keep the NUL strtox wrote */
	return p - (__xdata uint8_t *)uip_appdata;
}


static uint16_t tftp_ack_build(void)
{
	__xdata uint8_t *p = uip_appdata;

	*p++ = 0;
	*p++ = OP_ACK;
	*p++ = tftp.blk >> 8;
	*p = tftp.blk & 0xff;
	return 4;
}


/* Build DATA block tftp.blk (1-based) from the stored config text */
static uint16_t tftp_data_build(void)
{
	__xdata uint8_t *p = uip_appdata;
	__xdata uint16_t off = (tftp.blk - 1) * TFTP_BLOCK;
	__xdata uint16_t len = tftp.txlen - off;

	if (len > TFTP_BLOCK)
		len = TFTP_BLOCK;
	*p++ = 0;
	*p++ = OP_DATA;
	*p++ = tftp.blk >> 8;
	*p++ = tftp.blk & 0xff;
	/* FLASH_BUF_SIZE (512) == TFTP_BLOCK, so one bulk read covers it */
	flash_region.addr = CONFIG_START + off;
	flash_region.len = TFTP_BLOCK;
	flash_read_bulk(flash_buf);
	for (off = 0; off < len; off++)
		*p++ = flash_buf[off];
	tftp.last_len_short = len < TFTP_BLOCK;
	return len + 4;
}


static void tftp_send_request(void)
{
	/* The request must go to port 69; the reply comes from the
	 * server's TID, matched via the wildcard set on the next poll. */
	tftp.conn->rport = HTONS(TFTP_SERVER_PORT);
	uip_udp_send(tftp_req_build());
	tftp.req_sent = 1;
	tftp.tick_last = ticks_now();
}


/* Firmware download finished: verify the staged image the same way the
 * boot-time updater will, then reset to let it apply the image. */
static void tftp_fw_finish(void)
{
	__xdata uint32_t pos = FIRMWARE_UPLOAD_START;
	__xdata uint16_t i, j;
	__xdata uint8_t *bptr;

	if (tftp.addr != FIRMWARE_UPLOAD_START + TFTP_FW_SIZE) {
		tftp_abort("image size mismatch, expected a full 512KB image");
		return;
	}
	print_string("\nTransfer complete, verifying image");
	crc_value = 0;
	for (i = 0; i < TFTP_FW_SIZE / FLASH_BUF_SIZE; i++) {
		flash_region.addr = pos;
		flash_region.len = FLASH_BUF_SIZE;
		flash_read_bulk(flash_buf);
		bptr = flash_buf;
		for (j = 0; j < FLASH_BUF_SIZE; j++)
			crc16_bank1(bptr++);
		pos += FLASH_BUF_SIZE;
		if (i % 64 == 0)
			write_char('.');
	}
	if (crc_value != 0xb001) {
		tftp_abort("image CRC mismatch");
		return;
	}
	print_string("OK\nRebooting to apply the update\n");
	tftp_teardown();
	delay(200);
	reset_chip();
}


static void tftp_rx_data(void)
{
	__xdata uint8_t *p = uip_appdata;
	__xdata uint16_t blkno = ((uint16_t)p[2] << 8) | p[3];
	__xdata uint16_t len = uip_datalen() - 4;

	if (blkno == tftp.blk) {
		/* Duplicate of the last block: our ACK got lost */
		uip_udp_send(tftp_ack_build());
		return;
	}
	if (blkno != tftp.blk + 1)
		return;

	if (tftp.conn->rport == 0)
		tftp.conn->rport = UDPBUF->srcport;

	if (tftp.op == TFTP_OP_GET_FW) {
		if (tftp.addr + len > FIRMWARE_UPLOAD_START + TFTP_FW_SIZE) {
			tftp_abort("file larger than the staging area");
			return;
		}
		if (len) {
			if ((tftp.addr & (FLASH_SECTOR_SIZE - 1)) == 0) {
				flash_region.addr = tftp.addr;
				flash_sector_erase();
			}
			flash_region.addr = tftp.addr;
			flash_region.len = len;
			flash_write_bytes(p + 4);
			tftp.addr += len;
		}
	} else {
		/* Startup config: buffer in RAM, burn only when complete, so
		 * an aborted transfer cannot destroy the stored config. */
		__xdata uint16_t off = tftp.blk * TFTP_BLOCK;
		__xdata uint16_t i;
		if (off + len > CONFIG_LEN - 1) {
			tftp_abort("config larger than the config sector");
			return;
		}
		for (i = 0; i < len; i++)
			cfg_buf[off + i] = p[4 + i];
	}

	tftp.blk = blkno;
	tftp.state = T_RX;
	tftp.retries = 0;
	tftp.tick_last = ticks_now();
	uip_udp_send(tftp_ack_build());
	if ((tftp.blk & 0x3f) == 0)
		write_char('.');

	if (len < TFTP_BLOCK) {
		if (tftp.op == TFTP_OP_GET_FW) {
			/* Let the final ACK leave first; verify on the next poll */
			tftp.state = T_FIN;
		} else {
			__xdata uint16_t total = tftp.blk ? (tftp.blk - 1) * TFTP_BLOCK + len : 0;
			cfg_buf[total] = 0;
			flash_region.addr = CONFIG_START;
			flash_sector_erase();
			flash_region.addr = CONFIG_START;
			flash_region.len = total + 1;
			flash_write_bytes(cfg_buf);
			print_string("\nStartup config received (");
			itoa_short(total);
			print_string(" bytes). It takes effect on the next boot.\n");
			tftp_teardown();
		}
	}
}


static void tftp_rx_ack(void)
{
	__xdata uint8_t *p = uip_appdata;
	__xdata uint16_t blkno = ((uint16_t)p[2] << 8) | p[3];

	if (blkno != tftp.blk)
		return;

	if (tftp.conn->rport == 0)
		tftp.conn->rport = UDPBUF->srcport;

	if (tftp.state == T_REQ || tftp.state == T_TX) {
		if (tftp.state == T_TX && tftp.last_len_short) {
			print_string("\nStartup config sent (");
			itoa_short(tftp.txlen);
			print_string(" bytes)\n");
			tftp_teardown();
			return;
		}
		tftp.blk++;
		tftp.state = T_TX;
		tftp.retries = 0;
		tftp.tick_last = ticks_now();
		uip_udp_send(tftp_data_build());
	}
}


static void tftp_rx_error(void)
{
	__xdata uint8_t *p = uip_appdata;
	__xdata uint16_t len = uip_datalen();
	__xdata uint16_t i;

	print_string("\nTFTP server error: ");
	for (i = 4; i < len && p[i]; i++)
		write_char(p[i]);
	write_char('\n');
	tftp.addr = FIRMWARE_UPLOAD_START + 1;	/* force staging cleanup */
	tftp_abort("aborted by server");
}


void tftp_callback(uint16_t lport) __banked
{
	if (!tftp.conn || lport != HTONS(TFTP_CLIENT_PORT))
		return;

	if (uip_newdata()) {
		__xdata uint8_t *p = uip_appdata;
		if (uip_datalen() < 4)
			return;
		if (p[0] != 0)
			return;
		switch (p[1]) {
		case OP_DATA:
			if (tftp.op == TFTP_OP_PUT_CONFIG)
				return;
			tftp_rx_data();
			return;
		case OP_ACK:
			if (tftp.op != TFTP_OP_PUT_CONFIG)
				return;
			tftp_rx_ack();
			return;
		case OP_ERROR:
			tftp_rx_error();
			return;
		}
		return;
	}

	if (uip_poll()) {
		if (tftp.state == T_FIN) {
			tftp_fw_finish();
			return;
		}
		if (tftp.state == T_REQ && !tftp.req_sent) {
			tftp_send_request();
			return;
		}
		if (tftp.req_sent && tftp.conn->rport == HTONS(TFTP_SERVER_PORT)) {
			/* Request is on the wire; accept the reply from the
			 * server's transfer TID. */
			tftp.conn->rport = 0;
			return;
		}
		if (ticks_now() - tftp.tick_last > TFTP_TIMEOUT_TICKS) {
			tftp.retries++;
			if (tftp.retries > TFTP_MAX_RETRIES) {
				tftp_abort("timeout, no response from server");
				return;
			}
			tftp.tick_last = ticks_now();
			switch (tftp.state) {
			case T_REQ:
				tftp_send_request();
				return;
			case T_RX:
				uip_udp_send(tftp_ack_build());
				return;
			case T_TX:
				uip_udp_send(tftp_data_build());
				return;
			}
		}
	}
}
