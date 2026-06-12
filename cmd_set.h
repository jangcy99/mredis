#ifndef CMD_SET_H
#define CMD_SET_H
#include <pthread.h>
#include "mredis_types.h"
#include "mredis_core.h"

#define SET_BUCKETS 32

typedef struct	{
	pthread_mutex_t	mutex;
	uint32_t	member_count;
    uint32_t	n_buckets;
	uint64_t	data_offset;
	uint64_t	buckets[SET_BUCKETS];
} SetHeader;

typedef struct	{
	uint32_t	member_len;
	uint32_t	pad;
	uint64_t	member_offset;
	uint64_t	next_offset;
} SetEntry;

s_replyObject *cmd_sadd        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_srem        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_sismember   (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_scard       (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_smembers    (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_spop        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_srandmember (MRedisHandle *h, string_t *args[], uint32_t argc);

s_replyObject *cmd_sunion(MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_sinter(MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_sdiff(MRedisHandle *h, string_t *args[], uint32_t argc);

int drop_set(MRedisHandle *h, const void *key, uint32_t klen);
#endif
