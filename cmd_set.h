#ifndef CMD_SET_H
#define CMD_SET_H
#include <pthread.h>
#include "mredis_types.h"
#include "mredis_core.h"

typedef struct	{
	pthread_mutex_t	mutex;
	uint32_t	member_count;
    uint32_t	n_buckets;
	uint64_t	data_offset;
} SetHeader;

typedef struct	{
	uint32_t	member_len;
	uint32_t	pad;
	uint64_t	member_offset;
	uint64_t	next_offset;
} SetEntry;

s_replyObject *cmd_screate     (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_sdrop       (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_sadd        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_srem        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_sismember   (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_scard       (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_smembers    (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_spop        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_srandmember (MRedisHandle *h, string_t *args[], uint32_t argc);

#endif
