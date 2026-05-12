#ifndef CMD_KV_H
#define CMD_KV_H
/*
 * cmd_kv.h  –  KV 커맨드 (SET / GET / DEL)
 *
 *  SET  key value          → STATUS "OK"  | ERROR
 *  GET  key                → STRING value | NIL | ERROR
 *  DEL  key [key …]        → INTEGER 삭제 수
 */
#include "shm_types.h"
#include "shm_core.h"

s_replyObject *cmd_set(ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_get(ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_mset(ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_mget(ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_kdel(ShmHandle *h, string_t *args[], uint32_t argc);

#endif /* CMD_KV_H */
