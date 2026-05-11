#ifndef CMD_KEYS_H
#define CMD_KEYS_H
/*
 * cmd_keys.h  –  KV 커맨드 (SET / GET / DEL)
 *
 *  keys  pattern
 */
#include "shm_types.h"
#include "shm_core.h"

s_replyObject *cmd_keys(ShmHandle *h, string_t *args[], uint32_t argc);

#endif /* CMD_KEYS_H */
