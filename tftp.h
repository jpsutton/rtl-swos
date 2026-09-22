#ifndef __TFTP_H__
#define __TFTP_H__

#include <stdint.h>

/* Local UDP port of the client; the server's first reply carries its
 * transfer TID, which the connection is then locked to. */
#define TFTP_CLIENT_PORT 3069
#define TFTP_SERVER_PORT 69

/* Transfer operations */
#define TFTP_OP_GET_FW		1	/* file -> firmware staging area */
#define TFTP_OP_GET_CONFIG	2	/* file -> startup-config sector */
#define TFTP_OP_PUT_CONFIG	3	/* startup-config sector -> file */

#define TFTP_FNAME_SIZE 64

void tftp_init(void) __banked;
void tftp_begin(uint8_t op, __xdata const char *fname) __banked;
void tftp_callback(uint16_t lport) __banked;
uint8_t tftp_busy(void) __banked;

#endif
