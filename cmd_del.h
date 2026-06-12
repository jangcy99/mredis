#ifndef CMD_DEL_H
#define CMD_DEL_H
/*
 * cmd_del.h  –  범용 DEL 라우팅 테이블
 *
 * ┌──────────────────────────────────────────────────────────────┐
 * │  DEL key [key …]                                             │
 * │                                                              │
 * │  동작:                                                        │
 * │    각 key 에 대해 NameEntry.type 을 확인하고                 │
 * │    del_route_table[] 에 등록된 해당 타입의 drop 함수를 호출. │
 * │                                                              │
 * │  반환: INTEGER – 실제로 삭제된 key 수                         │
 * │        (없는 key 는 무시, 에러는 건너뜀)                     │
 * │                                                              │
 * │  새 ENTRY 타입 추가 시:                                      │
 * │    1. cmd_del.c 의 del_route_table[] 에 한 줄 추가           │
 * │    2. 해당 drop 함수를 DelDropFn 시그니처에 맞게 래핑        │
 * └──────────────────────────────────────────────────────────────┘
 */
#include "mredis_types.h"
#include "mredis_core.h"

/*
 * DEL 라우팅 테이블 엔트리.
 * drop_fn: bucket 잠금 해제 상태에서 호출.
 *   key/klen 은 삭제 대상 키.
 *   반환: SHM_OK(=0) 성공, 음수 실패
 */
typedef int (*DelDropFn)(MRedisHandle *h,
                          const void *key, uint32_t klen);

typedef struct {
    uint32_t   entry_type;   /* ENTRY_KV / ENTRY_ZSET / ENTRY_HASH … */
    const char *type_name;   /* 로그용 이름                           */
    DelDropFn  drop_fn;      /* 해당 타입 drop 함수 래퍼              */
} DelRouteEntry;

/* 범용 DEL – 모든 타입 삭제 */
int	register_cmd_del (const uint32_t entry_type, const char *type_name, DelDropFn func);
s_replyObject *cmd_del(MRedisHandle *h, string_t *args[], uint32_t argc);

/* 라우팅 테이블 직접 접근 (테스트/확장용) */
const DelRouteEntry *del_route_table_get(size_t *out_count);

#endif /* CMD_DEL_H */
