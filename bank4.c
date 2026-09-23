/*
 * BANK4 probe: proves the fourth code bank is mapped before anything
 * depends on it (`debug bank4`).
 */
#include <stdint.h>
#include "rtl837x_common.h"
#include "bank4.h"

#pragma codeseg BANK4
#pragma constseg BANK4

static __code const char bank4_msg[] = "BANK4 code and constants readable\n";

uint16_t bank4_probe(void) __banked
{
	print_string(bank4_msg);
	return 0x4b34;
}
