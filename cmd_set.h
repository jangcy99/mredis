#ifndef CMD_SET_H
#define CMD_SET_H

s_replyObject *cmd_screate     (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_sdrop       (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_sadd        (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_srem        (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_sismember   (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_scard       (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_smembers    (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_spop        (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_srandmember (ShmHandle *h, string_t *args[], uint32_t argc);

#endif
