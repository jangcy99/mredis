#ifndef CMD_KEYS_H
#define CMD_KEYS_H
#include "mredis_types.h"
#include "mredis_core.h"
s_replyObject *cmd_keys(MRedisHandle *h, string_t *args[], uint32_t argc);
#endif
