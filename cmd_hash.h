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
#include "mredis_types.h"
#include "mredis_core.h"

typedef struct {
    uint64_t next_offset;
	uint64_t field_offset;
    uint64_t val_offset;
	uint32_t field_len;
	uint32_t val_len;
} FieldEntry;

typedef struct {
    pthread_mutex_t mutex;
    uint64_t field_count;
	uint32_t n_buckets;
	uint32_t pad;
    /* 뒤이어 uint64_t field_buckets[n_buckets] */
} HashHeader;

s_replyObject *cmd_hcreate     (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hdrop       (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hset        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hget        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hdel        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hexists     (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hlen        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hgetall     (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hkeys       (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hvals       (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hincrby     (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_hincrbyfloat(MRedisHandle *h, string_t *args[], uint32_t argc);

int drop_hash(MRedisHandle *h, const void *key, uint32_t klen);
#endif /* CMD_HASH_H */
