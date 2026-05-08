#ifndef CMD_HASH_H
#define CMD_HASH_H
/*
 * cmd_hash.h  –  Hash Map 커맨드 (ENTRY_HASH)
 *
 *  HCREATE  key
 *  HDROP    key
 *  HSET     key field value [field value …]
 *  HGET     key field
 *  HDEL     key field [field …]
 *  HEXISTS  key field
 *  HLEN     key
 *  HGETALL  key
 *  HKEYS    key
 *  HVALS    key
 *  HINCRBY      key field delta
 *  HINCRBYFLOAT key field delta
 */
#include "shm_types.h"
#include "shm_core.h"

s_replyObject *cmd_hcreate     (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hdrop       (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hset        (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hget        (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hdel        (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hexists     (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hlen        (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hgetall     (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hkeys       (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hvals       (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hincrby     (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hincrbyfloat(ShmHandle *h, string_t *args[], uint32_t argc);

#endif /* CMD_HASH_H */
