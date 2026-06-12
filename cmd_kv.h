#ifndef CMD_KV_H
#define CMD_KV_H
/*
 * cmd_kv.h  –  SET / GET
 *  DEL 은 cmd_del.h / cmd_del.c 로 분리됨.
 */
#include "mredis_types.h"
#include "mredis_core.h"

typedef struct {
	uint64_t val_offset;
	uint32_t val_len;
	uint32_t pad;
} KVNode;

s_replyObject *cmd_set(MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_get(MRedisHandle *h, string_t *args[], uint32_t argc);

int drop_kv(MRedisHandle *h, const void *key, uint32_t klen);
#endif /* CMD_KV_H */
