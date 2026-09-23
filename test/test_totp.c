/*
 * test_totp.c - totp.c against the RFC 6238 SHA-1 test vectors (6-digit
 * truncation of the published 8-digit codes).
 */
#include "sdcc_shim.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "totp.h"
#include "support.h"

/* the line-editor edges support.c drives; unused here */
uint8_t l;
void cmd_editor_init(void) { }
void cmd_edit(void) { }

static uint32_t now;
uint32_t ntp_unix_now(void) { return now; }

static int ok(const char *code)
{
	uint8_t b[8];
	strcpy((char *)b, code);
	return totp_verify(b);
}

static int secret(const char *s)
{
	uint8_t b[64];
	strcpy((char *)b, s);
	return totp_set_secret(b);
}

int main(void)
{
	printf("== totp.c ==\n");
	totp_init();
	/* "12345678901234567890" */
	CHECK(secret("GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"), "the RFC 6238 secret decodes");
	CHECK(totp_keylen == 20, "to 20 bytes");
	now = 59;
	CHECK(!ok("287082"), "nothing verifies while TOTP is off");
	totp_enabled = 1;
	CHECK(ok("287082"), "T = 59: 94287082");
	CHECK(!ok("287083"), "a wrong code fails");
	CHECK(!ok("28708"), "a short code fails");
	now = 1111111109;
	CHECK(ok("081804"), "T = 1111111109: 07081804");
	now = 1111111109 + 30;
	CHECK(ok("081804"), "one step late still verifies");
	now = 1111111109 + 90;
	CHECK(!ok("081804"), "three steps late does not");
	now = 1234567890;
	CHECK(ok("005924"), "T = 1234567890: 89005924");
	now = 2000000000;
	CHECK(ok("279037"), "T = 2000000000: 69279037");
	now = 0;
	CHECK(!ok("005924"), "no clock: fails closed");

	now = 1234567890;
	CHECK(!secret("GEZDGNBVGY3TQOJQ1!"), "invalid characters refused");
	CHECK(!secret("GEZDGNBV"), "too short refused");
	CHECK(ok("005924") && totp_keylen == 20, "and the key in use is kept");
	CHECK(secret("gezdgnbvgy3tqojqgezdgnbvgy3tqojq"), "lower case accepted");

	printf("%d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
