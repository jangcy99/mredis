#ifndef CMD_BSET_H
#define CMD_BSET_H
/*
 * cmd_bset.h  –  Binary Sorted Set (BSET)  ── 고성능 시계열 특화 버전
 *
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │  설계 개요                                                           │
 * │                                                                     │
 * │  BSetHeader (SHM 힙)                                                │
 * │    └─ array_offset → BSetEntry[] (score 오름차순 정렬 배열)         │
 * │                       └─ offset  → 실제 데이터 (SHM 힙)            │
 * │                                                                     │
 * │  ── 읽기 최소화 전략 ────────────────────────────────────────────── │
 * │                                                                     │
 * │  1. pthread_rwlock (PROCESS_SHARED + ROBUST)                        │
 * │     읽기 커맨드: rdlock (복수 독자 동시 진입)                        │
 * │     쓰기 커맨드: wrlock (배타)                                       │
 * │                                                                     │
 * │  2. 시계열 증가 Append Fast-Path                                    │
 * │     score > arr[count-1].score → memmove 없이 O(1) 끝에 추가       │
 * │     10000/sec 이상의 단조 증가 입력에서 O(1) 삽입                   │
 * │                                                                     │
 * │  3. 쓰기 중 읽기 구조 분리                                          │
 * │     array_offset / count / capacity 는 wrlock 구간에서만 변경.     │
 * │     rdlock 구간에서는 이 세 필드를 스냅샷으로 읽어 독립 탐색.       │
 * │                                                                     │
 * │  커맨드 분류                                                         │
 * │  ────────────────────────────────────────────────────────────────── │
 * │  읽기(rdlock): BGET BRANGE BRANGEBYSCORE BCARD BRANK BCOUNT         │
 * │  쓰기(wrlock): BSET BDEL BPOPMIN BPOPMAX BDROP                     │
 * │                                                                     │
 * │  커맨드 시그니처 (기존과 동일)                                       │
 * │  ────────────────────────────────────────────────────────────────── │
 * │  BSET  key score value [score value …]  → INTEGER 신규 수           │
 * │  BGET  key score                        → STRING | NIL              │
 * │  BDEL  key score [score …]              → INTEGER 삭제 수           │
 * │  BRANGE      key start stop             → ARRAY [score val …]       │
 * │  BRANGEBYSCORE key min max [LIMIT …]    → ARRAY [score val …]       │
 * │  BCARD key                              → INTEGER                   │
 * │  BRANK key score                        → INTEGER | NIL             │
 * │  BCOUNT key min max                     → INTEGER                   │
 * │  BPOPMIN key [count]                    → ARRAY [score val …]       │
 * │  BPOPMAX key [count]                    → ARRAY [score val …]       │
 * │  BDROP key                              → STATUS "OK"               │
 * └─────────────────────────────────────────────────────────────────────┘
 */

#include "mredis_types.h"
#include "mredis_core.h"

/* ── ENTRY 타입 번호 ─────────────────────────────────────── */
#ifndef ENTRY_BSET
#define ENTRY_BSET  5u
#endif

/* ── 배열 확장 단위 ─────────────────────────────────────── */
#define BSET_CHUNK  1024u

/* ── 온디스크(SHM힙) 엔트리 ─────────────────────────────── */
typedef struct {
    uint64_t score;    /* 정렬 키               */
    uint64_t offset;   /* 실제 데이터 힙 오프셋  */
    uint32_t vlen;     /* 데이터 바이트 수       */
    uint32_t _pad;
} BSetEntry;           /* 24 bytes, 8-byte aligned */

/* ── BSetHeader (SHM 힙에 단독 할당) ────────────────────────
 *
 *  rwlock: PROCESS_SHARED + ROBUST
 *    - rdlock: BGET / BRANGE / BRANGEBYSCORE / BCARD / BRANK / BCOUNT
 *    - wrlock: BSET / BDEL / BPOPMIN / BPOPMAX / BDROP
 *
 *  array_offset / count / capacity 는 반드시 wrlock 구간에서만 수정.
 *  rdlock 구간에서는 세 필드를 지역 스냅샷으로 읽어 사용.
 * ─────────────────────────────────────────────────────────── */
typedef struct {
    uint64_t          array_offset;  /* BSetEntry[] 힙 오프셋          */
    uint64_t          count;         /* 현재 원소 수                   */
    uint64_t          capacity;      /* 할당 슬롯 수 (BSET_CHUNK 배수)  */
    pthread_rwlock_t  rwlock;        /* PROCESS_SHARED + ROBUST        */
} BSetHeader;

/* ── 공개 API ─────────────────────────────────────────────── */
s_replyObject *cmd_bset         (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_bget         (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_bdel         (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_brange       (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_brangebyscore(MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_bcard        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_brank        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_bcount       (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_bpopmin      (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_bpopmax      (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_bdrop        (MRedisHandle *h, string_t *args[], uint32_t argc);

/* 내부 조회 (단위테스트 / cmd_del 라우팅용) */
BSetHeader *core_bset_get(MRedisHandle *h, const void *key, uint32_t klen);

int drop_bset(MRedisHandle *h, const void *key, uint32_t klen);
#endif /* CMD_BSET_H */
