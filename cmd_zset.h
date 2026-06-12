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
#include "mredis_types.h"
#include "mredis_core.h"

/* ============================================================
 *  ZSet 파라미터
 * ============================================================ */

#define ZADD_NONE   0x00
#define ZADD_NX     0x01
#define ZADD_XX     0x02
#define ZADD_GT     0x04
#define ZADD_LT     0x08
#define ZADD_CH     0x10

#define ZRANGE_ASC  0
#define ZRANGE_DESC 1

/* ============================================================
 *  ZSetHeader / HashHeader 조회
 * ============================================================ */

s_replyObject *cmd_zcreate      (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zdrop        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zadd         (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zrem         (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zscore       (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zincrby      (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zrank        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zrevrank     (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zcard        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zcount       (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zrange       (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zrangebyscore(MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zpopmin      (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_zpopmax      (MRedisHandle *h, string_t *args[], uint32_t argc);

int drop_zset(MRedisHandle *h, const void *key, uint32_t klen);
#endif /* CMD_ZSET_H */
