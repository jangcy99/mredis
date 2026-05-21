#ifndef CMD_BSET_H
#define CMD_BSET_H
/*
 * cmd_bset.h  –  Binary Sorted Set (BSET)
 *
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │  설계 개요                                                           │
 * │                                                                     │
 * │  BSetHeader (SHM 힙)                                                │
 * │    └─ array_offset → BSetEntry[] (정렬된 배열, SHM 힙)              │
 * │                       └─ offset → 실제 데이터 (SHM 힙)              │
 * │                                                                     │
 * │  BSetEntry { uint64_t score; uint64_t offset; }                     │
 * │    - score 기준 오름차순 정렬 유지                                   │
 * │    - offset: 실제 데이터(value) 의 SHM 힙 오프셋                    │
 * │                                                                     │
 * │  메모리 레이아웃                                                     │
 * │    BSetHeader                                                        │
 * │      ├─ array_offset  → [BSetEntry × capacity]  (SHM 힙)            │
 * │      ├─ count         : 현재 원소 수                                 │
 * │      ├─ capacity      : 할당된 슬롯 수 (BSET_CHUNK 단위 확장)       │
 * │      └─ mutex         : PROCESS_SHARED + ROBUST                     │
 * │                                                                     │
 * │  삽입/삭제: bsearch → memmove 방식 O(log n) 탐색 + O(n) 이동        │
 * │  조회    : bsearch → O(log n)                                       │
 * │  확장    : count==capacity → heap_alloc(new) → memcpy → heap_free   │
 * │                                                                     │
 * │  커맨드 목록                                                         │
 * │  ──────────────────────────────────────────────────────────────────  │
 * │  BSET key score value [score value ...]                             │
 * │      → INTEGER: 새로 추가된 수 (갱신=0, 신규=1 per pair)            │
 * │                                                                     │
 * │  BGET key score                                                     │
 * │      → STRING: value  |  NIL                                        │
 * │                                                                     │
 * │  BDEL key score [score ...]                                         │
 * │      → INTEGER: 삭제된 수                                           │
 * │                                                                     │
 * │  BRANGE key start stop                                              │
 * │      → ARRAY: [score value score value ...]  (0-based index)        │
 * │                                                                     │
 * │  BRANGEBYSCORE key min max [LIMIT offset count]                     │
 * │      → ARRAY: [score value ...]                                     │
 * │                                                                     │
 * │  BCARD key                                                          │
 * │      → INTEGER: 원소 수                                             │
 * │                                                                     │
 * │  BRANK key score                                                    │
 * │      → INTEGER: 0-based rank  |  NIL                                │
 * │                                                                     │
 * │  BCOUNT key min max                                                 │
 * │      → INTEGER: score 범위 내 원소 수                               │
 * │                                                                     │
 * │  BPOPMIN key [count]                                                │
 * │      → ARRAY: [score value ...]                                     │
 * │                                                                     │
 * │  BPOPMAX key [count]                                                │
 * │      → ARRAY: [score value ...]                                     │
 * │                                                                     │
 * │  BDROP key                                                          │
 * │      → STATUS: "OK"                                                 │
 * └─────────────────────────────────────────────────────────────────────┘
 */

#include "shm_types.h"
#include "shm_core.h"

/* ── ENTRY 타입 번호 ─────────────────────────────────────── */
#ifndef ENTRY_BSET
#define ENTRY_BSET  5u
#endif

/* ── 배열 확장 단위 ─────────────────────────────────────── */
#define BSET_CHUNK  1024u   /* 1024개 단위로 alloc/realloc */

/* ── 온디스크(SHM힙) 엔트리 ─────────────────────────────── */
typedef struct {
    uint64_t score;    /* 정렬 키              */
    uint64_t offset;   /* 실제 데이터 힙 오프셋 */
    uint32_t vlen;     /* 데이터 바이트 수      */
    uint32_t _pad;
} BSetEntry;           /* 24 bytes, 8-byte aligned */

/* ── BSetHeader (SHM 힙에 단독 할당) ────────────────────── */
typedef struct {
    uint64_t        array_offset; /* BSetEntry[] 힙 오프셋         */
    uint32_t        count;        /* 현재 원소 수                  */
    uint32_t        capacity;     /* 할당 슬롯 수 (BSET_CHUNK 배수) */
    pthread_mutex_t mutex;        /* PROCESS_SHARED + ROBUST       */
} BSetHeader;

/* ── 공개 API ─────────────────────────────────────────────── */
s_replyObject *cmd_bset         (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_bget         (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_bdel         (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_brange       (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_brangebyscore(ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_bcard        (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_brank        (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_bcount       (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_bpopmin      (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_bpopmax      (ShmHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_bdrop        (ShmHandle *h, string_t *args[], uint32_t argc);

/* 내부 조회 (단위테스트/cmd_del 라우팅용) */
BSetHeader *core_bset_get(ShmHandle *h, const void *key, uint32_t klen);

#endif /* CMD_BSET_H */
