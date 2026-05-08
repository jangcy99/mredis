#ifndef CMD_DISPATCH_H
#define CMD_DISPATCH_H
/*
 * cmd_dispatch.h  –  커맨드 이름 → 함수 포인터 라우터
 *
 *  사용 예:
 *    string_t *args[4] = { &STR_LIT("SET"), &STR_LIT("key"), &STR_LIT("val"), NULL };
 *    s_replyObject *r  = cmd_dispatch(h, args, 3);
 *    reply_print(r, 0);
 *    reply_free(r);
 *
 *  등록된 커맨드 목록:
 *    KV   : SET GET DEL
 *    ZSet : ZCREATE ZDROP ZADD ZREM ZSCORE ZINCRBY ZRANK ZREVRANK
 *           ZCARD ZCOUNT ZRANGE ZRANGEBYSCORE ZPOPMIN ZPOPMAX
 *    Hash : HCREATE HDROP HSET HGET HDEL HEXISTS HLEN
 *           HGETALL HKEYS HVALS HINCRBY HINCRBYFLOAT
 */
#include "shm_types.h"
#include "shm_core.h"

/* 커맨드 함수 포인터 타입 */
typedef s_replyObject *(*CmdFunc)(ShmHandle *h,
                                  string_t  *args[],
                                  uint32_t   argc);

/* 커맨드 테이블 엔트리 */
typedef struct {
    const char *name;   /* 대소문자 무관 매칭 */
    CmdFunc     fn;
    uint32_t    min_argc; /* args[0] 포함 최소 인자 수 */
    const char *usage;
} CmdEntry;

/*
 * cmd_dispatch
 *   args[0] 의 커맨드 이름을 찾아 해당 함수를 호출한다.
 *   알 수 없는 커맨드이면 REPLY_ERROR 반환.
 */
s_replyObject *cmd_dispatch(ShmHandle *h,
                             string_t  *args[],
                             uint32_t   argc);

/*
 * cmd_table_get
 *   커맨드 테이블 전체를 반환 (등록된 커맨드 수도 함께).
 *   테스트 / CLI 자동완성 등에 활용.
 */
const CmdEntry *cmd_table_get(size_t *out_count);

#endif /* CMD_DISPATCH_H */
