/*
 * cmd_bset.c  –  Binary Sorted Set 구현
 *
 *  핵심 자료구조:
 *    BSetHeader.array_offset → BSetEntry[capacity]
 *    - score 기준 오름차순 정렬 유지
 *    - 삽입/삭제: bsearch + memmove
 *    - 확장: count==capacity → 새 배열(+BSET_CHUNK) 할당 → memcpy → 구배열 해제
 *    - 축소: count < capacity/2 이고 capacity>BSET_CHUNK → 절반 크기로 shrink
 *
 *  직렬화(serialize):
 *    모든 배열 접근은 bset_lock/bset_unlock 으로 보호.
 *    bucket_lock → 타입 확인 → unlock → bset_lock 순서 일관.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "shm_types.h"
#include "shm_core.h"
#include "cmd_bset.h"

/* ============================================================
 *  §1  내부 헬퍼 – mutex
 * ============================================================ */
static void bs_mutex_init(pthread_mutex_t *m)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&a,  PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}
static inline void bset_lock(BSetHeader *bh)
{
    int rc = pthread_mutex_lock(&bh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&bh->mutex);
}
static inline void bset_unlock(BSetHeader *bh)
{
    pthread_mutex_unlock(&bh->mutex);
}

/* ============================================================
 *  §2  내부 헬퍼 – BSetEntry 배열 접근자
 * ============================================================ */
static inline BSetEntry *bs_arr(ShmHandle *h, BSetHeader *bh)
{
    return (BSetEntry *)OFF2PTR(h, bh->array_offset);
}

/* bsearch 비교 함수: score 기준 */
static int bs_cmp_score(const void *a, const void *b)
{
    uint64_t sa = *(const uint64_t *)a;
    uint64_t sb = ((const BSetEntry *)b)->score;
    return (sa > sb) - (sa < sb);
}

/*
 * bs_lower_bound: score 보다 크거나 같은 첫 번째 인덱스 반환.
 * arr[0..count-1] 은 score 오름차순 정렬 상태.
 */
static uint64_t bs_lower_bound(const BSetEntry *arr, uint64_t count,
                                uint64_t score)
{
	if (arr[count - 1].score < score) return count;
    uint64_t lo = 0, hi = count;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (arr[mid].score < score) lo = mid + 1;
        else                        hi = mid;
    }
    return lo;
}

/*
 * bs_upper_bound: score 보다 큰 첫 번째 인덱스 반환.
 */
static uint64_t bs_upper_bound(const BSetEntry *arr, uint64_t count,
                                uint64_t score)
{
    uint64_t lo = 0, hi = count;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (arr[mid].score <= score) lo = mid + 1;
        else                          hi = mid;
    }
    return lo;
}

/* ============================================================
 *  §3  내부 헬퍼 – BSetHeader 생명주기
 * ============================================================ */

/* 타입 검사 (bucket 잠긴 상태) */
static int bset_type_check(ShmHandle *h, BucketEntry *bk,
                             const void *key, uint32_t klen,
                             uint64_t *out_ne)
{
    uint64_t ne = bucket_find_locked(h, bk, key, klen, 0, NULL);
    if (ne == OFFSET_NULL) { if (out_ne) *out_ne = OFFSET_NULL; return 1; }
    NameEntry *nep = (NameEntry *)OFF2PTR(h, ne);
    if (nep->type != ENTRY_BSET) return SHM_ERR_KEY_EXISTS;
    if (out_ne) *out_ne = ne;
    return 0;
}

/* BSetHeader + 초기 배열 생성 (bucket 잠긴 상태) */
static int bset_create_locked(ShmHandle *h, BucketEntry *bk,
                                const void *key, uint32_t klen)
{
    /* (a) BSetHeader 할당 */
    uint64_t bh_off = heap_alloc(h, sizeof(BSetHeader));
    if (bh_off == OFFSET_NULL) return SHM_ERR_NOMEM;

    BSetHeader *bh = (BSetHeader *)OFF2PTR(h, bh_off);
    memset(bh, 0, sizeof(BSetHeader));
    bs_mutex_init(&bh->mutex);

    /* (b) 초기 배열 (BSET_CHUNK 개 슬롯) */
    uint64_t arr_off = heap_alloc(h, BSET_CHUNK * sizeof(BSetEntry));
    if (arr_off == OFFSET_NULL) {
        pthread_mutex_destroy(&bh->mutex);
        heap_free(h, bh_off);
        return SHM_ERR_NOMEM;
    }
    memset(OFF2PTR(h, arr_off), 0, BSET_CHUNK * sizeof(BSetEntry));
    bh->array_offset = arr_off;
    bh->count        = 0;
    bh->capacity     = BSET_CHUNK;

    /* (c) NameEntry 등록 */
    uint64_t ne_off = nameentry_alloc(h, key, klen, ENTRY_BSET, bh_off);
    if (ne_off == OFFSET_NULL) {
        heap_free(h, arr_off);
        pthread_mutex_destroy(&bh->mutex);
        heap_free(h, bh_off);
        return SHM_ERR_NOMEM;
    }
    ((NameEntry *)OFF2PTR(h, ne_off))->next_offset = bk->head_offset;
    bk->head_offset = ne_off;
    LOG_TRACE("BSET: create key=%.*s", klen, (const char *)key);
    return SHM_OK;
}

/* 없으면 자동 생성 */
static BSetHeader *bset_get_or_create(ShmHandle *h,
                                       const void *key, uint32_t klen,
                                       int *err_out)
{
    BSetHeader *bh = core_bset_get(h, key, klen);
    if (bh) { if (err_out) *err_out = SHM_OK; return bh; }

    uint32_t     idx = shm_hash(key, klen);
    BucketEntry *bk  = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t ne_off = OFFSET_NULL;
    int chk = bset_type_check(h, bk, key, klen, &ne_off);
    if (chk == SHM_ERR_KEY_EXISTS) {
        pthread_mutex_unlock(&bk->mutex);
        if (err_out) *err_out = SHM_ERR_KEY_EXISTS;
        return NULL;
    }
    if (chk == 0) {
        bh = (BSetHeader *)OFF2PTR(h,
             ((NameEntry *)OFF2PTR(h, ne_off))->data_offset);
        pthread_mutex_unlock(&bk->mutex);
        if (err_out) *err_out = SHM_OK;
        return bh;
    }
    int r = bset_create_locked(h, bk, key, klen);
    pthread_mutex_unlock(&bk->mutex);
    if (r != SHM_OK) { if (err_out) *err_out = r; return NULL; }
    if (err_out) *err_out = SHM_OK;
    return core_bset_get(h, key, klen);
}

/* 공개 조회 */
BSetHeader *core_bset_get(ShmHandle *h, const void *key, uint32_t klen)
{
    uint64_t ne = bucket_find(h, shm_hash(key, klen), key, klen, ENTRY_BSET, NULL);
    if (ne == OFFSET_NULL) return NULL;
    return (BSetHeader *)OFF2PTR(h,
           ((NameEntry *)OFF2PTR(h, ne))->data_offset);
}

/* ============================================================
 *  §4  내부 헬퍼 – 배열 확장 / 축소 (bset_lock 구간 내 호출)
 *
 *  확장 정책: count == capacity → +BSET_CHUNK 슬롯
 *  축소 정책: count <  capacity/2 이고 capacity > BSET_CHUNK
 *             → capacity/2 로 shrink (최소 BSET_CHUNK 유지)
 * ============================================================ */
static int bs_grow(ShmHandle *h, BSetHeader *bh)
{
    uint64_t new_cap = bh->capacity + BSET_CHUNK;
    uint64_t new_off = heap_alloc(h, new_cap * sizeof(BSetEntry));
    if (new_off == OFFSET_NULL) return SHM_ERR_NOMEM;

    /* 기존 데이터 복사 후 구배열 해제 */
    memcpy(OFF2PTR(h, new_off), OFF2PTR(h, bh->array_offset),
           bh->count * sizeof(BSetEntry));
    /* 신규 슬롯 초기화 */
    memset((BSetEntry *)OFF2PTR(h, new_off) + bh->count, 0,
           (new_cap - bh->count) * sizeof(BSetEntry));

    heap_free(h, bh->array_offset);
    bh->array_offset = new_off;
    bh->capacity     = new_cap;
    LOG_TRACE("BSET grow: capacity=%llu", (unsigned long long)new_cap);
    return SHM_OK;
}

static void bs_shrink_if_needed(ShmHandle *h, BSetHeader *bh)
{
    if (bh->capacity <= BSET_CHUNK) return;
    if (bh->count >= bh->capacity / 2) return;

    uint64_t new_cap = bh->capacity / 2;
    if (new_cap < BSET_CHUNK) new_cap = BSET_CHUNK;
    if (new_cap <= bh->count) return;  /* 안전 확인 */

    uint64_t new_off = heap_alloc(h, new_cap * sizeof(BSetEntry));
    if (new_off == OFFSET_NULL) return; /* shrink 실패해도 동작엔 문제없음 */

    memcpy(OFF2PTR(h, new_off), OFF2PTR(h, bh->array_offset),
           bh->count * sizeof(BSetEntry));
    heap_free(h, bh->array_offset);
    bh->array_offset = new_off;
    bh->capacity     = new_cap;
    LOG_TRACE("BSET shrink: capacity=%llu", (unsigned long long)new_cap);
}

/* ============================================================
 *  §5  내부 헬퍼 – 단일 score 삽입/갱신 (bset_lock 구간 내 호출)
 *
 *  동일 score 가 이미 존재하면 value 만 갱신 → 반환 0
 *  신규 score 이면 정렬 위치에 삽입 → 반환 1
 *  실패 → 음수
 * ============================================================ */
static int bs_insert_one(ShmHandle *h, BSetHeader *bh,
                          uint64_t score,
                          const void *val, uint32_t vlen)
{
    BSetEntry *arr = bs_arr(h, bh);


    uint64_t   pos = bs_lower_bound(arr, bh->count, score);

    /* 동일 score 존재 → value 갱신 */
    if (pos < bh->count && arr[pos].score == score) {
        heap_free(h, arr[pos].offset);
        uint64_t vo = heap_alloc(h, vlen > 0 ? vlen : 1);
        if (vo == OFFSET_NULL) return SHM_ERR_NOMEM;
        if (vlen > 0) memcpy(OFF2PTR(h, vo), val, vlen);
        arr[pos].offset = vo;
        arr[pos].vlen   = vlen;
        return 0;   /* 갱신 */
    }

    /* 신규 삽입 – 배열 확장 필요? */
    if (bh->count >= bh->capacity) {
        int r = bs_grow(h, bh);
        if (r != SHM_OK) return r;
        arr = bs_arr(h, bh); /* 재조회 (grow 후 주소 변경) */
    }

    /* value 데이터 힙 할당 */
    uint64_t vo = heap_alloc(h, vlen > 0 ? vlen : 1);
    if (vo == OFFSET_NULL) return SHM_ERR_NOMEM;
    if (vlen > 0) memcpy(OFF2PTR(h, vo), val, vlen);

    /* pos 이후를 한 칸 뒤로 밀기 */
    if (pos < bh->count)
        memmove(&arr[pos + 1], &arr[pos], (bh->count - pos) * sizeof(BSetEntry));

    arr[pos].score  = score;
    arr[pos].offset = vo;
    arr[pos].vlen   = vlen;
    bh->count++;
    return 1;   /* 신규 */
}

/* ============================================================
 *  §6  내부 헬퍼 – 응답 배열 빌더
 * ============================================================ */
static int bs_append_triple(ShmHandle *h, s_replyObject *arr,
                              const BSetEntry *e)
{
    /* score → 십진 문자열 */
    char sbuf[32];
    int  sl = snprintf(sbuf, sizeof(sbuf), "%llu",
                       (unsigned long long)e->score);
    s_replyObject *rs = reply_string(sbuf, (size_t)(sl > 0 ? sl : 0));

    const char *vp = (e->vlen > 0)
                     ? (const char *)OFF2PTR(h, e->offset) : "";
    s_replyObject *rv = reply_string(vp, e->vlen);

    if (!rs || !rv) { reply_free(rs); reply_free(rv); return SHM_ERR_NOMEM; }
    reply_array_append(arr, rs);
    reply_array_append(arr, rv);
    return SHM_OK;
}

/* ============================================================
 *  §7  공개 커맨드 구현
 * ============================================================ */

/* ── BSET  key score value [score value ...] ──────────────
 *  반환: INTEGER – 새로 추가된 수 (갱신은 미포함)
 * ─────────────────────────────────────────────────────────── */
s_replyObject *cmd_bset(ShmHandle *h, string_t *args[], uint32_t argc)
{
    /* args: BSET key score val [score val ...] */
    if (argc < 4 || (argc - 2) % 2 != 0)
        return reply_error(SHM_ERR_ARGC,
            "usage: BSET key score value [score value ...]");

    const void *key  = args[1]->ptr; uint32_t klen = args[1]->len;
    if (klen == 0) return reply_error(SHM_ERR_INVAL, "key 비어있음");

    /* 타입 충돌 사전 검사 */
    {
        uint32_t     idx = shm_hash(key, klen);
        BucketEntry *bk  = core_get_bucket(h, idx);
        bucket_lock(h, idx);
        uint64_t any = bucket_find_locked(h, bk, key, klen, 0, NULL);
        if (any != OFFSET_NULL &&
            ((NameEntry *)OFF2PTR(h, any))->type != ENTRY_BSET) {
            pthread_mutex_unlock(&bk->mutex);
            return reply_error(SHM_ERR_KEY_EXISTS,
                               shm_strerror(SHM_ERR_KEY_EXISTS));
        }
        pthread_mutex_unlock(&bk->mutex);
    }

    int err = SHM_OK;
    BSetHeader *bh = bset_get_or_create(h, key, klen, &err);
    if (!bh) return reply_error(err, shm_strerror(err));

    bset_lock(bh);

    int64_t added = 0;
    for (uint32_t i = 2; i + 1 < argc; i += 2) {
        char *ep    = NULL;
        uint64_t score = (uint64_t)strtoull(args[i]->ptr, &ep, 10);
        if (ep == args[i]->ptr) {
            bset_unlock(bh);
            return reply_error(SHM_ERR_PARSE, "score 변환 실패");
        }
        int r = bs_insert_one(h, bh, score,
                              args[i+1]->ptr, args[i+1]->len);
        if (r < 0) {
            bset_unlock(bh);
            return reply_error(r, shm_strerror(r));
        }
        if (r == 1) added++;
    }

    bset_unlock(bh);
    return reply_integer(added);
}

/* ── BGET  key score ──────────────────────────────────────
 *  반환: STRING value  |  NIL
 * ─────────────────────────────────────────────────────────── */
s_replyObject *cmd_bget(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3) return reply_error(SHM_ERR_ARGC, "usage: BGET key score");

    BSetHeader *bh = core_bset_get(h, args[1]->ptr, args[1]->len);
    if (!bh) return reply_nil();

    char *ep     = NULL;
    uint64_t score = (uint64_t)strtoull(args[2]->ptr, &ep, 10);
    if (ep == args[2]->ptr) return reply_error(SHM_ERR_PARSE, "score 변환 실패");

    bset_lock(bh);
    BSetEntry *arr = bs_arr(h, bh);
    /* bsearch */
    BSetEntry *found = (BSetEntry *)bsearch(&score, arr, bh->count,
                                             sizeof(BSetEntry), bs_cmp_score);
    if (!found) { bset_unlock(bh); return reply_nil(); }

    const char *vp = found->vlen > 0
                     ? (const char *)OFF2PTR(h, found->offset) : "";
    s_replyObject *r = reply_string(vp, found->vlen);
    bset_unlock(bh);
    return r;
}

/* ── BDEL  key score [score ...] ──────────────────────────
 *  반환: INTEGER – 삭제된 수
 * ─────────────────────────────────────────────────────────── */
s_replyObject *cmd_bdel(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3) return reply_error(SHM_ERR_ARGC,
        "usage: BDEL key score [score ...]");

    BSetHeader *bh = core_bset_get(h, args[1]->ptr, args[1]->len);
    if (!bh) return reply_integer(0);

    bset_lock(bh);
    int64_t removed = 0;

    for (uint32_t a = 2; a < argc; a++) {
        char *ep     = NULL;
        uint64_t score = (uint64_t)strtoull(args[a]->ptr, &ep, 10);
        if (ep == args[a]->ptr) continue;

        BSetEntry *arr   = bs_arr(h, bh);
        uint64_t   pos   = bs_lower_bound(arr, bh->count, score);
        if (pos >= bh->count || arr[pos].score != score) continue;

        heap_free(h, arr[pos].offset);

        /* 해당 위치 이후를 앞으로 당기기 */
        if (pos + 1 < bh->count)
            memmove(&arr[pos], &arr[pos + 1],
                    (bh->count - pos - 1) * sizeof(BSetEntry));

        bh->count--;
        removed++;
    }

    bs_shrink_if_needed(h, bh);
    bset_unlock(bh);
    return reply_integer(removed);
}

/* ── BRANGE  key start stop ───────────────────────────────
 *  0-based index, 음수 허용.
 *  반환: ARRAY [score value score value ...]
 * ─────────────────────────────────────────────────────────── */
s_replyObject *cmd_brange(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 4) return reply_error(SHM_ERR_ARGC,
        "usage: BRANGE key start stop");

    int64_t start = strtoll(args[2]->ptr, NULL, 10);
    int64_t stop  = strtoll(args[3]->ptr, NULL, 10);

    BSetHeader *bh = core_bset_get(h, args[1]->ptr, args[1]->len);
    if (!bh) return reply_array(0);

    bset_lock(bh);
    int64_t len = (int64_t)bh->count;
    if (len == 0) { bset_unlock(bh); return reply_array(0); }

    /* 음수 인덱스 정규화 */
    if (start < 0) start = len + start;
    if (stop  < 0) stop  = len + stop;
    if (start < 0) start = 0;
    if (stop >= len) stop = len - 1;
    if (start > stop) { bset_unlock(bh); return reply_array(0); }

    int64_t       need = stop - start + 1;
    s_replyObject *arr = reply_array((size_t)(need * 2));
    if (!arr) { bset_unlock(bh); return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM)); }

	BSetEntry *earr = (BSetEntry*)malloc (sizeof (BSetEntry) * len);
    memcpy (earr, bs_arr(h, bh), sizeof (BSetEntry) * len);
    bset_unlock(bh);
    for (int64_t i = start; i <= stop; i++) {
        if (bs_append_triple(h, arr, &earr[i]) != SHM_OK) {
            reply_free(arr); bset_unlock(bh);
			free (earr);
            return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
        }
    }
	free (earr);
    return arr;
}

/* ── BRANGEBYSCORE  key min max [LIMIT offset count] ──────
 *  반환: ARRAY [score value ...]  (오름차순)
 * ─────────────────────────────────────────────────────────── */
s_replyObject *cmd_brangebyscore(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 4) return reply_error(SHM_ERR_ARGC,
        "usage: BRANGEBYSCORE key min max [LIMIT offset count]");

    uint64_t mn = (uint64_t)strtoull(args[2]->ptr, NULL, 10);
    uint64_t mx = (uint64_t)strtoull(args[3]->ptr, NULL, 10);
    int64_t  loff = 0, lcnt = -1;

    for (uint32_t i = 4; i < argc; i++) {
        if (!strcasecmp(args[i]->ptr, "LIMIT") && i + 2 < argc) {
            loff = strtoll(args[i+1]->ptr, NULL, 10);
            lcnt = strtoll(args[i+2]->ptr, NULL, 10);
            i += 2;
        }
    }

    BSetHeader *bh = core_bset_get(h, args[1]->ptr, args[1]->len);
    if (!bh || mn > mx) return reply_array(0);

    bset_lock(bh);
	uint32_t	count = bh->count;
    BSetEntry *earr  = (BSetEntry*)malloc (sizeof (BSetEntry) * count);
    memcpy (earr, bs_arr(h, bh), sizeof (BSetEntry) * count);
	bset_unlock(bh);
	
    uint64_t   lo    = bs_lower_bound(earr, bh->count, mn);
    uint64_t   hi    = bs_upper_bound(earr, bh->count, mx);

    if (lo >= hi) { return reply_array(0); }

    s_replyObject *arr = reply_array((size_t)((hi - lo) * 2));
    if (!arr) { return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM)); }

    int64_t skipped = 0, collected = 0;
    for (uint64_t i = lo; i < hi; i++) {
        if (skipped < loff) { skipped++; continue; }
        if (lcnt >= 0 && collected >= lcnt) break;
        if (bs_append_triple(h, arr, &earr[i]) != SHM_OK) {
            reply_free(arr);
            return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
        }
        collected++;
    }
    return arr;
}

/* ── BCARD  key ───────────────────────────────────────────
 *  반환: INTEGER
 * ─────────────────────────────────────────────────────────── */
s_replyObject *cmd_bcard(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: BCARD key");
    BSetHeader *bh = core_bset_get(h, args[1]->ptr, args[1]->len);
    if (!bh) return reply_integer(0);
    bset_lock(bh);
    int64_t cnt = (int64_t)bh->count;
    bset_unlock(bh);
    return reply_integer(cnt);
}

/* ── BRANK  key score ─────────────────────────────────────
 *  반환: INTEGER 0-based rank  |  NIL
 * ─────────────────────────────────────────────────────────── */
s_replyObject *cmd_brank(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3) return reply_error(SHM_ERR_ARGC, "usage: BRANK key score");

    BSetHeader *bh = core_bset_get(h, args[1]->ptr, args[1]->len);
    if (!bh) return reply_nil();

    char *ep     = NULL;
    uint64_t score = (uint64_t)strtoull(args[2]->ptr, &ep, 10);
    if (ep == args[2]->ptr) return reply_error(SHM_ERR_PARSE, "score 변환 실패");

    bset_lock(bh);
    BSetEntry *arr = bs_arr(h, bh);
    uint64_t   pos = bs_lower_bound(arr, bh->count, score);
    int found = (pos < bh->count && arr[pos].score == score);
    bset_unlock(bh);

    return found ? reply_integer((int64_t)pos) : reply_nil();
}

/* ── BCOUNT  key min max ──────────────────────────────────
 *  반환: INTEGER
 * ─────────────────────────────────────────────────────────── */
s_replyObject *cmd_bcount(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 4) return reply_error(SHM_ERR_ARGC, "usage: BCOUNT key min max");

    uint64_t mn = (uint64_t)strtoull(args[2]->ptr, NULL, 10);
    uint64_t mx = (uint64_t)strtoull(args[3]->ptr, NULL, 10);
    if (mn > mx) return reply_integer(0);

    BSetHeader *bh = core_bset_get(h, args[1]->ptr, args[1]->len);
    if (!bh) return reply_integer(0);

    bset_lock(bh);
	uint32_t	capacity = bh->capacity;
	uint32_t	count = bh->count;
	BSetEntry *arr = malloc (sizeof (BSetEntry) * capacity);     /* 할당 슬롯 수 (BSET_CHUNK 배수) */
    memcpy (arr, bs_arr(h, bh), sizeof (BSetEntry) * count);
    bset_unlock(bh);
    uint64_t   lo  = bs_lower_bound(arr, bh->count, mn);
    uint64_t   hi  = bs_upper_bound(arr, bh->count, mx);
    int64_t    cnt = (hi >= lo) ? (int64_t)(hi - lo) : 0;
	free (arr);
    return reply_integer(cnt);
}

/* ── BPOPMIN / BPOPMAX  key [count] ──────────────────────
 *  반환: ARRAY [score value ...]
 * ─────────────────────────────────────────────────────────── */
static s_replyObject *bpop_impl(ShmHandle *h, string_t *args[],
                                 uint32_t argc, int from_min)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC,
        from_min ? "usage: BPOPMIN key [count]"
                 : "usage: BPOPMAX key [count]");
    int64_t count = 1;
    if (argc >= 3) {
        count = strtoll(args[2]->ptr, NULL, 10);
        if (count <= 0) return reply_error(SHM_ERR_INVAL, "count > 0 필요");
    }

    BSetHeader *bh = core_bset_get(h, args[1]->ptr, args[1]->len);
    if (!bh) return reply_array(0);

    bset_lock(bh);
    if (count > (int64_t)bh->count) count = (int64_t)bh->count;

    s_replyObject *arr = reply_array((size_t)(count * 2));
    if (!arr) { bset_unlock(bh); return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM)); }

    BSetEntry *earr = bs_arr(h, bh);
    for (int64_t i = 0; i < count; i++) {
        /* pop 대상 인덱스 */
        uint64_t idx = from_min ? 0 : (bh->count - 1);

        /* 결과 수집 */
        char sbuf[32];
        int  sl = snprintf(sbuf, sizeof(sbuf), "%llu",
                           (unsigned long long)earr[idx].score);
        reply_array_append(arr, reply_string(sbuf, (size_t)(sl > 0 ? sl : 0)));
        const char *vp = earr[idx].vlen > 0
                         ? (const char *)OFF2PTR(h, earr[idx].offset) : "";
        reply_array_append(arr, reply_string(vp, earr[idx].vlen));

        /* 실제 삭제 */
        heap_free(h, earr[idx].offset);
        if (from_min && bh->count > 1)
            memmove(&earr[0], &earr[1], (bh->count - 1) * sizeof(BSetEntry));
        bh->count--;
        /* earr 포인터는 불변 (배열 주소 변경 없음) */
    }
    bs_shrink_if_needed(h, bh);
    bset_unlock(bh);
    return arr;
}

s_replyObject *cmd_bpopmin(ShmHandle *h, string_t *args[], uint32_t argc)
    { return bpop_impl(h, args, argc, 1); }
s_replyObject *cmd_bpopmax(ShmHandle *h, string_t *args[], uint32_t argc)
    { return bpop_impl(h, args, argc, 0); }

/* ── BDROP  key ───────────────────────────────────────────
 *  반환: STATUS "OK"  |  ERROR
 * ─────────────────────────────────────────────────────────── */
s_replyObject *cmd_bdrop(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: BDROP key");
    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;

    uint32_t     idx = shm_hash(key, klen);
    BucketEntry *bk  = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t prev   = OFFSET_NULL;
    uint64_t ne_off = bucket_find_locked(h, bk, key, klen, ENTRY_BSET, &prev);
    if (ne_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return reply_error(SHM_ERR_NOT_FOUND, shm_strerror(SHM_ERR_NOT_FOUND));
    }
    NameEntry  *ne    = (NameEntry *)OFF2PTR(h, ne_off);
    uint64_t    bh_off = ne->data_offset;
    BSetHeader *bh     = (BSetHeader *)OFF2PTR(h, bh_off);

    if (prev == OFFSET_NULL) bk->head_offset = ne->next_offset;
    else ((NameEntry *)OFF2PTR(h, prev))->next_offset = ne->next_offset;
    nameentry_free(h, ne_off);
    pthread_mutex_unlock(&bk->mutex);   /* bucket unlock 먼저 */

    /* 데이터 해제 */
    bset_lock(bh);
    BSetEntry *arr = bs_arr(h, bh);
    for (uint64_t i = 0; i < bh->count; i++)
        heap_free(h, arr[i].offset);
    heap_free(h, bh->array_offset);
    bset_unlock(bh);
    pthread_mutex_destroy(&bh->mutex);
    heap_free(h, bh_off);

    LOG_TRACE("BDROP: '%.*s' 완료", klen, (const char *)key);
    return reply_ok();
}
