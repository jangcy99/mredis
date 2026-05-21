/*
 * cmd_del.c  –  범용 DEL 라우팅 테이블 구현
 *
 *  기존 cmd_kv.c 의 DEL 은 ENTRY_KV 만 삭제했다.
 *  이 파일은 NameEntry.type 을 먼저 읽고 해당 타입의
 *  drop 함수를 호출하는 라우팅 레이어를 추가한다.
 *
 *  ┌──────────────────────────────────────────────────┐
 *  │  DEL key [key …]                                 │
 *  │       │                                          │
 *  │       ▼  bucket lock → NameEntry.type 확인       │
 *  │       │                                          │
 *  │  ┌────┴──────────────────────────┐               │
 *  │  │  del_route_table[]            │               │
 *  │  │  ENTRY_KV   → drop_kv()      │               │
 *  │  │  ENTRY_ZSET → drop_zset()    │               │
 *  │  │  ENTRY_HASH → drop_hash()    │               │
 *  │  └───────────────────────────────┘               │
 *  │       │                                          │
 *  │       ▼  bucket unlock                           │
 *  │  removed++                                       │
 *  └──────────────────────────────────────────────────┘
 *
 *  새 타입(예: ENTRY_ZHSET=4) 추가 방법
 *  ───────────────────────────────────────────────────
 *  1. drop_zhset() 래퍼 함수 작성 (아래 패턴 참고)
 *  2. del_route_table[] 에 한 줄 추가:
 *       { ENTRY_ZHSET, "ZHSET", drop_zhset }
 *  ───────────────────────────────────────────────────
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "shm_types.h"
#include "shm_core.h"
#include "cmd_zset.h"
#include "cmd_hash.h"
#include "cmd_bset.h"
#include "cmd_del.h"

/* ============================================================
 *  §1  타입별 drop 래퍼
 *
 *  각 래퍼는 DelDropFn 시그니처를 만족하면서
 *  내부적으로 기존 cmd_zdrop / cmd_hdrop 의 핵심 로직을
 *  직접 수행한다.
 *
 *  cmd_zdrop / cmd_hdrop 을 string_t 래핑 없이 직접 호출하면
 *  불필요한 args[] 배열 구성이 필요하므로,
 *  각 타입의 실제 해제 로직을 인라인으로 재구현한다.
 *  (코드 중복을 피하려면 내부 _drop_xxx_locked() 헬퍼를
 *   cmd_zset.c / cmd_hash.c 에서 공개하는 방법도 있지만,
 *   기존 파일 무수정 원칙을 지키기 위해 래퍼 방식을 사용한다.)
 * ============================================================ */

/* ── ENTRY_KV drop ──────────────────────────────────────────
 *  bucket 을 lock 한 상태에서 NameEntry 를 bucket chain 에서
 *  제거하고 KVNode + value 를 힙에서 해제한다.
 * ─────────────────────────────────────────────────────────── */
static int drop_kv(ShmHandle *h, const void *key, uint32_t klen)
{
    uint32_t     idx  = shm_hash(key, klen);
    BucketEntry *bk   = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t prev   = OFFSET_NULL;
    uint64_t ne_off = bucket_find_locked(h, bk, key, klen, ENTRY_KV, &prev);
    if (ne_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return SHM_ERR_NOT_FOUND;
    }
    NameEntry *ne = (NameEntry *)OFF2PTR(h, ne_off);
    KVNode    *kv = (KVNode    *)OFF2PTR(h, ne->data_offset);

    if (prev == OFFSET_NULL) bk->head_offset = ne->next_offset;
    else ((NameEntry *)OFF2PTR(h, prev))->next_offset = ne->next_offset;

    heap_free(h, kv->val_offset);
    heap_free(h, ne->data_offset);
    nameentry_free(h, ne_off);
    pthread_mutex_unlock(&bk->mutex);

    LOG_TRACE("DEL(KV): '%.*s'", klen, (const char *)key);
    return SHM_OK;
}

/* ── ENTRY_ZSET drop ────────────────────────────────────────
 *  ① bucket lock → NameEntry 체인에서 제거 → bucket unlock
 *  ② ZSetHeader.mutex lock → 모든 SkipNode 해제 → unlock
 *  ③ ZSetHeader 힙 해제
 *  (cmd_zdrop 과 동일한 순서, lock 역전 없음)
 * ─────────────────────────────────────────────────────────── */
static int drop_zset(ShmHandle *h, const void *key, uint32_t klen)
{
    uint32_t     idx = shm_hash(key, klen);
    BucketEntry *bk  = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t prev   = OFFSET_NULL;
    uint64_t ne_off = bucket_find_locked(h, bk, key, klen, ENTRY_ZSET, &prev);
    if (ne_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return SHM_ERR_NOT_FOUND;
    }
    NameEntry  *ne     = (NameEntry  *)OFF2PTR(h, ne_off);
    uint64_t    zsh_off = ne->data_offset;
    ZSetHeader *zsh     = (ZSetHeader *)OFF2PTR(h, zsh_off);

    if (prev == OFFSET_NULL) bk->head_offset = ne->next_offset;
    else ((NameEntry *)OFF2PTR(h, prev))->next_offset = ne->next_offset;
    nameentry_free(h, ne_off);
    pthread_mutex_unlock(&bk->mutex);   /* bucket unlock 먼저 */

    /* SkipList 노드 순회 해제 */
    int rc = pthread_mutex_lock(&zsh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&zsh->mutex);

    uint64_t cur = core_sn(h, zsh->head_offset)->forward[0];
    while (cur != OFFSET_NULL) {
        uint64_t nx = core_sn(h, cur)->forward[0];
        sl_node_free(h, cur);
        cur = nx;
    }
    heap_free(h, zsh->head_offset);   /* sentinel 해제 */
    pthread_mutex_unlock(&zsh->mutex);
    pthread_mutex_destroy(&zsh->mutex);
    heap_free(h, zsh_off);

    LOG_TRACE("DEL(ZSET): '%.*s'", klen, (const char *)key);
    return SHM_OK;
}

/* ── ENTRY_HASH drop ────────────────────────────────────────
 *  ① bucket lock → NameEntry 제거 → bucket unlock
 *  ② HashHeader.mutex lock → 모든 FieldEntry 해제 → unlock
 *  ③ HashHeader 힙 해제
 *  (cmd_hdrop 과 동일한 순서)
 * ─────────────────────────────────────────────────────────── */

/* hash_drop_fields_locked 는 cmd_hash.c 내부 static 이므로
 * 동일 로직을 여기서 인라인으로 수행한다.              */
static void drop_hash_fields(ShmHandle *h, HashHeader *hh)
{
    uint64_t *bkts = hh_field_buckets(hh);
    for (uint32_t i = 0; i < hh->n_buckets; i++) {
        uint64_t cur = bkts[i];
        while (cur != OFFSET_NULL) {
            FieldEntry *fe = (FieldEntry *)OFF2PTR(h, cur);
            uint64_t    nxt = fe->next_offset;
            heap_free(h, fe->field_offset);
            heap_free(h, fe->val_offset);
            heap_free(h, cur);
            cur = nxt;
        }
        bkts[i] = OFFSET_NULL;
    }
    hh->field_count = 0;
}

static int drop_hash(ShmHandle *h, const void *key, uint32_t klen)
{
    uint32_t     idx = shm_hash(key, klen);
    BucketEntry *bk  = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t prev   = OFFSET_NULL;
    uint64_t ne_off = bucket_find_locked(h, bk, key, klen, ENTRY_HASH, &prev);
    if (ne_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return SHM_ERR_NOT_FOUND;
    }
    NameEntry  *ne    = (NameEntry  *)OFF2PTR(h, ne_off);
    uint64_t    hh_off = ne->data_offset;
    HashHeader *hh     = (HashHeader *)OFF2PTR(h, hh_off);

    if (prev == OFFSET_NULL) bk->head_offset = ne->next_offset;
    else ((NameEntry *)OFF2PTR(h, prev))->next_offset = ne->next_offset;
    nameentry_free(h, ne_off);
    pthread_mutex_unlock(&bk->mutex);   /* bucket unlock 먼저 */

    int rc = pthread_mutex_lock(&hh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&hh->mutex);
    drop_hash_fields(h, hh);
    pthread_mutex_unlock(&hh->mutex);
    pthread_mutex_destroy(&hh->mutex);
    heap_free(h, hh_off);

    LOG_TRACE("DEL(HASH): '%.*s'", klen, (const char *)key);
    return SHM_OK;
}

static int drop_bset(ShmHandle *h, const void *key, uint32_t klen)
{
    uint32_t     idx = shm_hash(key, klen);
    BucketEntry *bk  = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t prev   = OFFSET_NULL;
    uint64_t ne_off = bucket_find_locked(h, bk, key, klen, ENTRY_BSET, &prev);
    if (ne_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return SHM_ERR_NOT_FOUND;
    }
    NameEntry  *ne    = (NameEntry *)OFF2PTR(h, ne_off);
    uint64_t    bh_off = ne->data_offset;
    BSetHeader *bh     = (BSetHeader *)OFF2PTR(h, bh_off);

    if (prev == OFFSET_NULL) bk->head_offset = ne->next_offset;
    else ((NameEntry *)OFF2PTR(h, prev))->next_offset = ne->next_offset;
    nameentry_free(h, ne_off);
    pthread_mutex_unlock(&bk->mutex);

    int rc = pthread_mutex_lock(&bh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&bh->mutex);

    BSetEntry *arr = (BSetEntry *)OFF2PTR(h, bh->array_offset);
    for (uint64_t i = 0; i < bh->count; i++)
        heap_free(h, arr[i].offset);
    heap_free(h, bh->array_offset);

    pthread_mutex_unlock(&bh->mutex);
    pthread_mutex_destroy(&bh->mutex);
    heap_free(h, bh_off);

    LOG_TRACE("DEL(BSET): '%.*s'", klen, (const char *)key);
    return SHM_OK;
}

/* ============================================================
 *  §2  DEL 라우팅 테이블
 *
 *  새 타입 추가 시 이 테이블에 한 줄만 추가하면 된다.
 * ============================================================ */
static const DelRouteEntry g_del_route[] = {
    { ENTRY_KV,   "KV",   drop_kv   },
    { ENTRY_ZSET, "ZSET", drop_zset },
    { ENTRY_HASH, "HASH", drop_hash },
    { ENTRY_BSET, "BSET", drop_bset },
    /*
     * 향후 추가 예시:
     * { ENTRY_ZHSET, "ZHSET", drop_zhset },
     * { ENTRY_LIST,  "LIST",  drop_list  },
     * { ENTRY_SET,   "SET",   drop_set   },
     */
};

static const size_t g_del_route_count =
    sizeof(g_del_route) / sizeof(g_del_route[0]);

const DelRouteEntry *del_route_table_get(size_t *out_count)
{
    if (out_count) *out_count = g_del_route_count;
    return g_del_route;
}

/* ============================================================
 *  §3  cmd_del  –  범용 DEL 구현
 *
 *  알고리즘:
 *    각 key 에 대해
 *      1. bucket lock → NameEntry.type 읽기 → bucket unlock
 *      2. del_route_table 에서 type 에 맞는 drop_fn 검색
 *      3. drop_fn(h, key, klen) 호출
 *      4. SHM_OK 이면 removed++
 *
 *  type 을 읽는 lock 과 drop_fn 내부 lock 이 분리되어 있지만
 *  이는 기존 ZDROP / HDROP 도 동일한 패턴이며,
 *  두 번 bucket_find 를 하더라도 정확성은 보장된다:
 *  - 다른 프로세스가 DEL 과 동시에 같은 key 를 삭제하면
 *    두 번째 bucket_find 가 OFFSET_NULL 을 반환해 안전하게 종료.
 *  - 타입이 바뀌는 일은 없다 (같은 key 에 다른 타입 삽입 불가).
 * ============================================================ */
s_replyObject *cmd_del(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2)
        return reply_error(SHM_ERR_ARGC, "usage: DEL key [key …]");

    int64_t removed = 0;

    for (uint32_t a = 1; a < argc; a++) {
        const void *key  = args[a]->ptr;
        uint32_t    klen = args[a]->len;
        if (klen == 0) continue;

        /* ── step 1: type 조회 (짧은 lock) ─────────────────── */
        uint32_t     idx  = shm_hash(key, klen);
        BucketEntry *bk   = core_get_bucket(h, idx);
        bucket_lock(h, idx);
        uint64_t ne_off = bucket_find_locked(h, bk, key, klen, 0, NULL);
        uint32_t type   = 0;
        if (ne_off != OFFSET_NULL)
            type = ((NameEntry *)OFF2PTR(h, ne_off))->type;
        pthread_mutex_unlock(&bk->mutex);

        if (ne_off == OFFSET_NULL) {
            LOG_TRACE("DEL: '%.*s' 없음 – skip", klen, (const char *)key);
            continue;   /* 없는 키는 무시 (Redis 호환) */
        }

        /* ── step 2: 라우팅 테이블에서 drop_fn 검색 ─────────── */
        DelDropFn   drop_fn   = NULL;
        const char *type_name = "UNKNOWN";
        for (size_t i = 0; i < g_del_route_count; i++) {
            if (g_del_route[i].entry_type == type) {
                drop_fn   = g_del_route[i].drop_fn;
                type_name = g_del_route[i].type_name;
                break;
            }
        }
        if (!drop_fn) {
            LOG_WARN("DEL: '%.*s' 알 수 없는 타입 %u – skip",
                     klen, (const char *)key, type);
            continue;
        }

        /* ── step 3: drop 실행 ─────────────────────────────── */
        int rc = drop_fn(h, key, klen);
        if (rc == SHM_OK) {
            removed++;
            LOG_TRACE("DEL: '%.*s' (%s) 삭제 완료", klen, (const char *)key, type_name);
        } else if (rc == SHM_ERR_NOT_FOUND) {
            /* step 1과 step 3 사이에 다른 프로세스가 먼저 삭제 */
            LOG_TRACE("DEL: '%.*s' 경쟁 삭제 – 무시", klen, (const char *)key);
        } else {
            LOG_ERR("DEL: '%.*s' (%s) drop 실패 rc=%d",
                    klen, (const char *)key, type_name, rc);
        }
    }

    return reply_integer(removed);
}
