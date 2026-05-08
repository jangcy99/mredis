#ifndef CMD_ZSET_H
#define CMD_ZSET_H
/*
 * cmd_zset.h  –  Sorted Set 커맨드
 *
 *  ZCREATE  key
 *  ZDROP    key
 *  ZADD     key [NX|XX] [GT|LT] [CH] score member [score member …]
 *  ZREM     key member [member …]
 *  ZSCORE   key member
 *  ZINCRBY  key delta member
 *  ZRANK    key member
 *  ZREVRANK key member
 *  ZCARD    key
 *  ZCOUNT   key min max
 *  ZRANGE   key start stop [REV]
 *  ZRANGEBYSCORE  key min max [REV] [LIMIT offset count]
 *  ZPOPMIN  key [count]
 *  ZPOPMAX  key [count]
 */
#include "shm_types.h"
#include "shm_core.h"

s_replyObject *cmd_zcreate      (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zdrop        (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zadd         (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zrem         (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zscore       (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zincrby      (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zrank        (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zrevrank     (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zcard        (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zcount       (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zrange       (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zrangebyscore(ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zpopmin      (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zpopmax      (ShmHandle *h, string_t *args[], uint32_t argc);

#endif /* CMD_ZSET_H */
