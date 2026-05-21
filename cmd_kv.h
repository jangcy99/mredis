#ifndef CMD_KV_H
#define CMD_KV_H
/*
 * cmd_kv.h  –  SET / GET
 *  DEL 은 cmd_del.h / cmd_del.c 로 분리됨.
 */
#include "shm_types.h"
#include "shm_core.h"

s_replyObject *cmd_set(ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_get(ShmHandle *h, string_t *args[], uint32_t argc);

#endif /* CMD_KV_H */
