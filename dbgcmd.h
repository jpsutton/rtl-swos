#ifndef __DBGCMD_H__
#define __DBGCMD_H__

#include <stdint.h>

/* debug operations, carried in the command tree's ->lo */
#define DBG_REG_RD	1
#define DBG_REG_WR	2
#define DBG_SDS_RD	3
#define DBG_SDS_WR	4
#define DBG_PHY_RD	5
#define DBG_PHY_WR	6
#define DBG_X_RD	7
#define DBG_X_TEST	8
#define DBG_GPIO	9
#define DBG_RND		10
#define DBG_FL_ID	11
#define DBG_FL_UID	12
#define DBG_FL_SEC	13

void debug_run(uint8_t op) __banked;

#endif
