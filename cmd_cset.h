#ifndef CMD_CSET_H
#define CMD_CSET_H
/*
 * cmd_cset.h  –  Chunk Sorted Set (CSET)
 *
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │  BSET(배열 기반) 와의 차이                                           │
 * │    BSET: 단일 정렬 배열, memmove 삽입/삭제                          │
 * │    CSET: 청크 연결 리스트, 삭제=비트 플래그, BCOMPACT 재정리        │
 * │                                                                     │
 * │  메모리 레이아웃                                                     │
 * │  CSetHeader                                                         │
 * │    ├─ chunk_head  → CSetChunk ↔ CSetChunk ↔ … (score 오름차순)     │
 * │    ├─ chunk_tail  → 마지막 CSetChunk                               │
 * │    ├─ chunk_count : 청크 수                                         │
 * │    ├─ total_count : live 원소 수                                    │
 * │    └─ rwlock      : PROCESS_SHARED                                 │
 * │                                                                     │
 * │  CSetChunk                                                          │
 * │    ├─ entries[CSET_CHUNK] : CSetEntry 배열 (score 오름차순)         │
 * │    ├─ deleted_bits[16]    : 삭제 비트마스크 (1024비트)              │
 * │    │    bit i = 1 → entries[i] 삭제됨 (value 이미 heap_free)       │
 * │    ├─ count   : 전체 슬롯 수 (삭제 포함)                           │
 * │    ├─ live    : 유효 원소 수                                        │
 * │    ├─ min_score / max_score                                         │
 * │    ├─ prev_off / next_off                                           │
 * │                                                                     │
 * │  삽입 전략                                                           │
 * │    Fast-Path: score > tail.max_score → tail 끝에 O(1) (시계열)     │
 * │    일반: 청크 min/max 탐색 → 청크 내 lower_bound + memmove          │
 * │    청크 꽉 참: 절반 분할(split) 후 삽입                            │
 * │                                                                     │
 * │  삭제 전략                                                           │
 * │    deleted_bits 비트 set + value heap_free → O(log n)              │
 * │    자동 병합: live < CSET_MERGE_THRESH 이면 인접 청크와 병합        │
 * │                                                                     │
 * │  CCOMPACT: 삭제 슬롯 물리 제거 + 청크 재구성                        │
 * │                                                                     │
 * │  읽기(rdlock): CGET CRANGE CRANGEBYSCORE CCARD CRANK CCOUNT        │
 * │  쓰기(wrlock): CSET CDEL CPOPMIN CPOPMAX CDROP CCOMPACT            │
 * └─────────────────────────────────────────────────────────────────────┘
 */

#include "mredis_types.h"
#include "mredis_core.h"

/* ── ENTRY 타입 번호 ─────────────────────────────────────── */
#ifndef ENTRY_CSET
#define ENTRY_CSET  6u
#endif

#define CSET_CHUNK        1024u
#define CSET_DEL_WORDS    (CSET_CHUNK / 64u)   /* 16 × uint64_t = 1024비트 */
#define CSET_MERGE_THRESH (CSET_CHUNK / 4u)    /* live < 256 → 병합 검토   */

/* ── 온디스크 엔트리 ─────────────────────────────────────── */
typedef struct {
    uint64_t score;    /* 정렬 키               */
    uint64_t offset;   /* 실제 데이터 힙 오프셋  */
    uint32_t vlen;     /* 데이터 바이트 수       */
    uint32_t _pad;
} CSetEntry;           /* 24 bytes, 8-byte aligned */

/* ── 청크 ────────────────────────────────────────────────── */
typedef struct {
    uint64_t  prev_off;
    uint64_t  next_off;
    uint64_t  min_score;
    uint64_t  max_score;
    uint32_t  count;                       /* 슬롯 수 (삭제 포함) */
    uint32_t  live;                        /* 유효 원소 수        */
    uint64_t  deleted_bits[CSET_DEL_WORDS];
    CSetEntry entries[CSET_CHUNK];
} CSetChunk;

/* 삭제 비트 조작 */
#define CS_DEL_SET(c,i) ((c)->deleted_bits[(i)/64] |=  (1ULL<<((i)%64)))
#define CS_DEL_CLR(c,i) ((c)->deleted_bits[(i)/64] &= ~(1ULL<<((i)%64)))
#define CS_DEL_GET(c,i) (((c)->deleted_bits[(i)/64]>>((i)%64))&1ULL)

/* ── CSetHeader ─────────────────────────────────────────── */
typedef struct {
    uint64_t         chunk_head;
    uint64_t         chunk_tail;
    uint64_t         chunk_count;
    uint64_t         total_count;  /* live 원소 수 */
    pthread_rwlock_t rwlock;       /* PROCESS_SHARED */
} CSetHeader;

/* ── 공개 API ─────────────────────────────────────────────── */
s_replyObject *cmd_cset         (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_cget         (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_cdel         (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_crange       (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_crangebyscore(MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_ccard        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_crank        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_ccount       (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_cpopmin      (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_cpopmax      (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_cdrop        (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_ccompact     (MRedisHandle *h, string_t *args[], uint32_t argc);

CSetHeader *core_cset_get(MRedisHandle *h, const void *key, uint32_t klen);

int drop_cset(MRedisHandle *h, const void *key, uint32_t klen);
#endif /* CMD_CSET_H */
