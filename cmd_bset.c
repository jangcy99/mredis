/*
 * cmd_bset.c  –  Binary Sorted Set (BSET) — 고성능 시계열 특화 버전
 *
 * ── 읽기 최소화 핵심 전략 3가지 ──────────────────────────────────────
 *
 *  [전략 1] pthread_rwlock_t (PROCESS_SHARED + ROBUST)
 *    읽기 커맨드 6개: rdlock → 복수 독자 동시 진입, 쓰기 블로킹 없음
 *    쓰기 커맨드 5개: wrlock → 배타
 *
 *  [전략 2] 시계열 증가 Append Fast-Path
 *    score > arr[count-1].score 조건이면:
 *      memmove 없이 arr[count] 에 직접 기록 → O(1) 삽입
 *    10,000/sec 이상 단조 증가 입력에서 삽입 비용 O(n) → O(1)
 *
 *  [전략 3] rdlock 구간의 스냅샷 읽기
 *    rdlock 진입 후 array_offset / count 를 지역변수로 스냅샷.
 *    탐색(lower_bound, bsearch)은 스냅샷으로 수행 → wrlock 불필요.
 *    wrlock 구간에서 array_offset 이 변경(grow/shrink)되더라도
 *    rdlock 구간의 스냅샷은 이전 유효 배열을 가리키므로 safe.
 *    (heap_free 는 wrlock 구간 밖에서 하지 않으므로 dangling 없음)
 *
 * ── Lock 순서 규칙 ──────────────────────────────────────────────────
 *    bucket_lock → rdlock/wrlock 순서 일관 유지 (역전 금지)
 *    bucket_lock 구간에서는 rwlock 을 절대 잡지 않음.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "mredis_types.h"
#include "mredis_core.h"
#include "cmd_bset.h"

/* ============================================================
 *  §1  rwlock 헬퍼
 * ============================================================ */
static void bs_rwlock_init(pthread_rwlock_t *rw)
{
    pthread_rwlockattr_t a;
    pthread_rwlockattr_init(&a);
    pthread_rwlockattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    pthread_rwlock_init(rw, &a);
    pthread_rwlockattr_destroy(&a);
}

static inline void bs_rdlock(BSetHeader *bh)
{
    /* rwlock은 robust 미지원 → EOWNERDEAD 발생 시 재초기화 필요 없음
     * (쓰기 프로세스 종료 시 커널이 자동 해제) */
    pthread_rwlock_rdlock(&bh->rwlock);
}
static inline void bs_wrlock(BSetHeader *bh)
{
    pthread_rwlock_wrlock(&bh->rwlock);
}
static inline void bs_unlock(BSetHeader *bh)
{
    pthread_rwlock_unlock(&bh->rwlock);
}

/* ============================================================
 *  §2  BSetEntry 배열 접근자 / 이진 탐색
 * ============================================================ */
static inline BSetEntry *bs_arr(MRedisHandle *h, BSetHeader *bh)
{
    return (BSetEntry *)OFF2PTR(h, bh->array_offset);
}

/* bsearch 비교 (score 기준) */
static int bs_cmp(const void *a, const void *b)
{
    uint64_t sa = *(const uint64_t *)a;
    uint64_t sb = ((const BSetEntry *)b)->score;
    return (sa > sb) - (sa < sb);
}

/* score 보다 크거나 같은 첫 번째 인덱스 */
static uint64_t bs_lower_bound(const BSetEntry *arr, uint64_t n,
                                uint64_t score)
{
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (arr[mid].score < score) lo = mid + 1;
        else                        hi = mid;
    }
    return lo;
}

/* score 보다 큰 첫 번째 인덱스 */
static uint64_t bs_upper_bound(const BSetEntry *arr, uint64_t n,
                                uint64_t score)
{
    uint64_t lo = 0, hi = n;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (arr[mid].score <= score) lo = mid + 1;
        else                          hi = mid;
    }
    return lo;
}

/* ============================================================
 *  §3  BSetHeader 생명주기
 * ============================================================ */
static int bset_type_check(MRedisHandle *h, BucketEntry *bk,
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

static int bset_create_locked(MRedisHandle *h, BucketEntry *bk,
                                const void *key, uint32_t klen)
{
    uint64_t bh_off = heap_alloc(h, sizeof(BSetHeader));
    if (bh_off == OFFSET_NULL) return SHM_ERR_NOMEM;

    BSetHeader *bh = (BSetHeader *)OFF2PTR(h, bh_off);
    memset(bh, 0, sizeof(BSetHeader));
    bs_rwlock_init(&bh->rwlock);

    uint64_t arr_off = heap_alloc(h, BSET_CHUNK * sizeof(BSetEntry));
    if (arr_off == OFFSET_NULL) {
        pthread_rwlock_destroy(&bh->rwlock);
        heap_free(h, bh_off);
        return SHM_ERR_NOMEM;
    }
    memset(OFF2PTR(h, arr_off), 0, BSET_CHUNK * sizeof(BSetEntry));
    bh->array_offset = arr_off;
    bh->count        = 0;
    bh->capacity     = BSET_CHUNK;

    uint64_t ne_off = nameentry_alloc(h, key, klen, ENTRY_BSET, bh_off);
    if (ne_off == OFFSET_NULL) {
        heap_free(h, arr_off);
        pthread_rwlock_destroy(&bh->rwlock);
        heap_free(h, bh_off);
        return SHM_ERR_NOMEM;
    }
    ((NameEntry *)OFF2PTR(h, ne_off))->next_offset = bk->head_offset;
    bk->head_offset = ne_off;
    return SHM_OK;
}

static BSetHeader *bset_get_or_create(MRedisHandle *h,
                                       const void *key, uint32_t klen,
                                       int *err_out)
{
    BSetHeader *bh = core_bset_get(h, key, klen);
    if (bh) { if (err_out) *err_out = SHM_OK; return bh; }

    uint32_t     idx = mredis_hash(key, klen)%((MRedisHeader*)(h->base))->hash_table_size;
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

BSetHeader *core_bset_get(MRedisHandle *h, const void *key, uint32_t klen)
{
    uint64_t ne = bucket_find(h, mredis_hash(key, klen)%((MRedisHeader*)(h->base))->hash_table_size,
                               key, klen, ENTRY_BSET, NULL);
    if (ne == OFFSET_NULL) return NULL;
    return (BSetHeader *)OFF2PTR(h,
           ((NameEntry *)OFF2PTR(h, ne))->data_offset);
}

/* ============================================================
 *  §4  배열 확장 / 축소 (wrlock 구간 내 호출)
 * ============================================================ */
static int bs_grow(MRedisHandle *h, BSetHeader *bh)
{
    uint64_t new_cap = bh->capacity + BSET_CHUNK;
    uint64_t new_off = heap_alloc(h, new_cap * sizeof(BSetEntry));
    if (new_off == OFFSET_NULL) return SHM_ERR_NOMEM;

    memcpy(OFF2PTR(h, new_off), OFF2PTR(h, bh->array_offset),
           bh->count * sizeof(BSetEntry));
    memset((BSetEntry *)OFF2PTR(h, new_off) + bh->count, 0,
           (new_cap - bh->count) * sizeof(BSetEntry));

    uint64_t old_off  = bh->array_offset;
    bh->array_offset  = new_off;   /* 원자적으로 교체 */
    bh->capacity      = new_cap;
    heap_free(h, old_off);         /* old는 wrlock 구간이므로 안전 */
    return SHM_OK;
}

static void bs_shrink_if_needed(MRedisHandle *h, BSetHeader *bh)
{
    if (bh->capacity <= BSET_CHUNK) return;
    if (bh->count >= bh->capacity / 2) return;

    uint64_t new_cap = bh->capacity / 2;
    if (new_cap < BSET_CHUNK) new_cap = BSET_CHUNK;
    if (new_cap <= bh->count) return;

    uint64_t new_off = heap_alloc(h, new_cap * sizeof(BSetEntry));
    if (new_off == OFFSET_NULL) return;

    memcpy(OFF2PTR(h, new_off), OFF2PTR(h, bh->array_offset),
           bh->count * sizeof(BSetEntry));

    uint64_t old_off = bh->array_offset;
    bh->array_offset = new_off;
    bh->capacity     = new_cap;
    heap_free(h, old_off);
}

/* ============================================================
 *  §5  단일 score 삽입/갱신 (wrlock 구간 내 호출)
 *
 *  [전략 2] Append Fast-Path:
 *    조건: count > 0  &&  score > arr[count-1].score
 *    → 배열 끝에 바로 추가, memmove 없음, O(1)
 *    시계열 단조 증가 입력에서 memmove 비용 완전 제거.
 *
 *  일반 경로: lower_bound 탐색 + memmove
 *
 *  반환: 1=신규, 0=갱신, 음수=에러
 * ============================================================ */
static int bs_insert_one(MRedisHandle *h, BSetHeader *bh,
                          uint64_t score,
                          const void *val, uint32_t vlen)
{
    BSetEntry *arr = bs_arr(h, bh);

    /* ── 동일 score 존재 확인 (fast check: 마지막 원소) ── */
    if (bh->count > 0 && arr[bh->count - 1].score == score) {
        /* 가장 최근 삽입과 score 일치 → value 갱신 */
        heap_free(h, arr[bh->count - 1].offset);
        uint64_t vo = heap_alloc(h, vlen > 0 ? vlen : 1);
        if (vo == OFFSET_NULL) return SHM_ERR_NOMEM;
        if (vlen > 0) memcpy(OFF2PTR(h, vo), val, vlen);
        arr[bh->count - 1].offset = vo;
        arr[bh->count - 1].vlen   = vlen;
        return 0;
    }

    /* ── [전략 2] Append Fast-Path ── */
    if (bh->count == 0 ||
        score > arr[bh->count - 1].score) {

        /* 배열 확장 필요? */
        if (bh->count >= bh->capacity) {
            int r = bs_grow(h, bh);
            if (r != SHM_OK) return r;
            arr = bs_arr(h, bh);
        }
        uint64_t vo = heap_alloc(h, vlen > 0 ? vlen : 1);
        if (vo == OFFSET_NULL) return SHM_ERR_NOMEM;
        if (vlen > 0) memcpy(OFF2PTR(h, vo), val, vlen);
        arr[bh->count].score  = score;
        arr[bh->count].offset = vo;
        arr[bh->count].vlen   = vlen;
        bh->count++;
        return 1;   /* 신규, O(1) */
    }

    /* ── 일반 경로: 이진 탐색 + memmove ── */
    uint64_t pos = bs_lower_bound(arr, bh->count, score);

    /* 동일 score 존재 → value 갱신 */
    if (pos < bh->count && arr[pos].score == score) {
        heap_free(h, arr[pos].offset);
        uint64_t vo = heap_alloc(h, vlen > 0 ? vlen : 1);
        if (vo == OFFSET_NULL) return SHM_ERR_NOMEM;
        if (vlen > 0) memcpy(OFF2PTR(h, vo), val, vlen);
        arr[pos].offset = vo;
        arr[pos].vlen   = vlen;
        return 0;
    }

    if (bh->count >= bh->capacity) {
        int r = bs_grow(h, bh);
        if (r != SHM_OK) return r;
        arr = bs_arr(h, bh);
    }
    uint64_t vo = heap_alloc(h, vlen > 0 ? vlen : 1);
    if (vo == OFFSET_NULL) return SHM_ERR_NOMEM;
    if (vlen > 0) memcpy(OFF2PTR(h, vo), val, vlen);

    if (pos < bh->count)
        memmove(&arr[pos + 1], &arr[pos],
                (bh->count - pos) * sizeof(BSetEntry));
    arr[pos].score  = score;
    arr[pos].offset = vo;
    arr[pos].vlen   = vlen;
    bh->count++;
    return 1;
}

/* ============================================================
 *  §6  응답 배열 빌더 (rdlock / wrlock 구간 모두 사용)
 * ============================================================ */
static int bs_append_pair(MRedisHandle *h, s_replyObject *arr,
                           const BSetEntry *e)
{
    char sbuf[24];
    int  sl = snprintf(sbuf, sizeof(sbuf), "%llu",
                       (unsigned long long)e->score);
    s_replyObject *rs = reply_string(sbuf, (size_t)(sl > 0 ? sl : 0));
    const char    *vp = (e->vlen > 0)
                        ? (const char *)OFF2PTR(h, e->offset) : "";
    s_replyObject *rv = reply_string(vp, e->vlen);
    if (!rs || !rv) { reply_free(rs); reply_free(rv); return SHM_ERR_NOMEM; }
    reply_array_append(arr, rs);
    reply_array_append(arr, rv);
    return SHM_OK;
}

/* ============================================================
 *  §7  BSET  key score value [score value …]
 *  [wrlock] 신규=1, 갱신=0 per pair
 * ============================================================ */
s_replyObject *cmd_bset(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 4 || (argc - 2) % 2 != 0)
        return reply_error(SHM_ERR_ARGC,
            "usage: BSET key score value [score value ...]");

    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;
    if (klen == 0) return reply_error(SHM_ERR_INVAL, "key 비어있음");

    /* 타입 충돌 사전 검사 (bucket lock → unlock) */
    {
        uint32_t     idx = mredis_hash(key, klen)%((MRedisHeader*)(h->base))->hash_table_size;
        BucketEntry *bk  = core_get_bucket(h, idx);
        bucket_lock(h, idx);
        uint64_t any = bucket_find_locked(h, bk, key, klen, 0, NULL);
        if (any != OFFSET_NULL &&
            ((NameEntry *)OFF2PTR(h, any))->type != ENTRY_BSET) {
            pthread_mutex_unlock(&bk->mutex);
            return reply_error(SHM_ERR_KEY_EXISTS,
                               mredis_strerror(SHM_ERR_KEY_EXISTS));
        }
        pthread_mutex_unlock(&bk->mutex);
    }

    int err = SHM_OK;
    BSetHeader *bh = bset_get_or_create(h, key, klen, &err);
    if (!bh) return reply_error(err, mredis_strerror(err));

    /* wrlock: 쓰기 배타 */
    bs_wrlock(bh);

    int64_t added = 0;
    for (uint32_t i = 2; i + 1 < argc; i += 2) {
        char    *ep    = NULL;
        uint64_t score = strtoull(args[i]->ptr, &ep, 10);
        if (ep == args[i]->ptr) {
            bs_unlock(bh);
            return reply_error(SHM_ERR_PARSE, "score 변환 실패");
        }
        int r = bs_insert_one(h, bh, score,
                              args[i+1]->ptr, args[i+1]->len);
        if (r < 0) { bs_unlock(bh); return reply_error(r, mredis_strerror(r)); }
        if (r == 1) added++;
    }

    bs_unlock(bh);
    return reply_integer(added);
}

/* ============================================================
 *  §8  BGET  key score
 *  [rdlock] 복수 독자 동시 진입 가능
 * ============================================================ */
s_replyObject *cmd_bget(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3) return reply_error(SHM_ERR_ARGC, "usage: BGET key score");

    BSetHeader *bh = core_bset_get(h, args[1]->ptr, args[1]->len);
    if (!bh) return reply_nil();

    char    *ep    = NULL;
    uint64_t score = strtoull(args[2]->ptr, &ep, 10);
    if (ep == args[2]->ptr) return reply_error(SHM_ERR_PARSE, "score 변환 실패");

    /* [전략 3] rdlock + 스냅샷 읽기 */
    bs_rdlock(bh);
    uint64_t   snap_off   = bh->array_offset;
    uint64_t   snap_count = bh->count;
    bs_unlock(bh);   /* 스냅샷 확보 후 즉시 해제 */

    /* lock 없이 탐색 — 탐색 중 wrlock이 들어올 수 없으므로 안전
     * (snap_off가 가리키는 배열은 wrlock 구간 밖에서 heap_free되지 않음) */
    BSetEntry *arr   = (BSetEntry *)OFF2PTR(h, snap_off);
    BSetEntry *found = (BSetEntry *)bsearch(&score, arr, snap_count,
                                             sizeof(BSetEntry), bs_cmp);
    if (!found) return reply_nil();

    /* value 복사 (배열은 여전히 유효) */
    const char *vp = found->vlen > 0
                     ? (const char *)OFF2PTR(h, found->offset) : "";
    return reply_string(vp, found->vlen);
}

/* ============================================================
 *  §9  BDEL  key score [score …]
 *  [wrlock]
 * ============================================================ */
s_replyObject *cmd_bdel(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3)
        return reply_error(SHM_ERR_ARGC, "usage: BDEL key score [score ...]");

    BSetHeader *bh = core_bset_get(h, args[1]->ptr, args[1]->len);
    if (!bh) return reply_integer(0);

    bs_wrlock(bh);
    int64_t removed = 0;

    for (uint32_t a = 2; a < argc; a++) {
        char    *ep    = NULL;
        uint64_t score = strtoull(args[a]->ptr, &ep, 10);
        if (ep == args[a]->ptr) continue;

        BSetEntry *arr = bs_arr(h, bh);
        uint64_t   pos = bs_lower_bound(arr, bh->count, score);
        if (pos >= bh->count || arr[pos].score != score) continue;

        heap_free(h, arr[pos].offset);
        if (pos + 1 < bh->count)
            memmove(&arr[pos], &arr[pos + 1],
                    (bh->count - pos - 1) * sizeof(BSetEntry));
        bh->count--;
        removed++;
    }

    bs_shrink_if_needed(h, bh);
    bs_unlock(bh);
    return reply_integer(removed);
}

/* ============================================================
 *  §10  BRANGE  key start stop
 *  [rdlock] 스냅샷 후 lock-free 탐색, 복사
 * ============================================================ */
s_replyObject *cmd_brange(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 4)
        return reply_error(SHM_ERR_ARGC, "usage: BRANGE key start stop");

    int64_t start = strtoll(args[2]->ptr, NULL, 10);
    int64_t stop  = strtoll(args[3]->ptr, NULL, 10);

    BSetHeader *bh = core_bset_get(h, args[1]->ptr, args[1]->len);
    if (!bh) return reply_array(0);

    /* [전략 3] 스냅샷 획득 후 즉시 unlock */
    bs_rdlock(bh);
    uint64_t snap_off   = bh->array_offset;
    uint64_t snap_count = bh->count;
    bs_unlock(bh);

    if (snap_count == 0) return reply_array(0);

    int64_t len = (int64_t)snap_count;
    if (start < 0) start = len + start;
    if (stop  < 0) stop  = len + stop;
    if (start < 0) start = 0;
    if (stop >= len) stop = len - 1;
    if (start > stop) return reply_array(0);

    int64_t       need = stop - start + 1;
    s_replyObject *arr = reply_array((size_t)(need * 2));
    if (!arr) return reply_error(SHM_ERR_NOMEM, mredis_strerror(SHM_ERR_NOMEM));

    BSetEntry *earr = (BSetEntry *)OFF2PTR(h, snap_off);
    for (int64_t i = start; i <= stop; i++) {
        if (bs_append_pair(h, arr, &earr[i]) != SHM_OK) {
            reply_free(arr);
            return reply_error(SHM_ERR_NOMEM, mredis_strerror(SHM_ERR_NOMEM));
        }
    }
    return arr;
}

/* ============================================================
 *  §11  BRANGEBYSCORE  key min max [LIMIT offset count]
 *  [rdlock] 스냅샷 후 lock-free 탐색
 * ============================================================ */
s_replyObject *cmd_brangebyscore(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 4)
        return reply_error(SHM_ERR_ARGC,
            "usage: BRANGEBYSCORE key min max [LIMIT offset count]");

    uint64_t mn  = strtoull(args[2]->ptr, NULL, 10);
    uint64_t mx  = strtoull(args[3]->ptr, NULL, 10);
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

    bs_rdlock(bh);
    uint64_t snap_off   = bh->array_offset;
    uint64_t snap_count = bh->count;
    bs_unlock(bh);

    BSetEntry *earr = (BSetEntry *)OFF2PTR(h, snap_off);
    uint64_t   lo   = bs_lower_bound(earr, snap_count, mn);
    uint64_t   hi   = bs_upper_bound(earr, snap_count, mx);
    if (lo >= hi) return reply_array(0);

    s_replyObject *arr = reply_array((size_t)((hi - lo) * 2));
    if (!arr) return reply_error(SHM_ERR_NOMEM, mredis_strerror(SHM_ERR_NOMEM));

    int64_t skipped = 0, collected = 0;
    for (uint64_t i = lo; i < hi; i++) {
        if (skipped < loff) { skipped++; continue; }
        if (lcnt >= 0 && collected >= lcnt) break;
        if (bs_append_pair(h, arr, &earr[i]) != SHM_OK) {
            reply_free(arr);
            return reply_error(SHM_ERR_NOMEM, mredis_strerror(SHM_ERR_NOMEM));
        }
        collected++;
    }
    return arr;
}

/* ============================================================
 *  §12  BCARD  key
 *  [rdlock] count 단순 읽기 — 스냅샷 후 즉시 unlock
 * ============================================================ */
s_replyObject *cmd_bcard(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: BCARD key");
    BSetHeader *bh = core_bset_get(h, args[1]->ptr, args[1]->len);
    if (!bh) return reply_integer(0);
    bs_rdlock(bh);
    int64_t cnt = (int64_t)bh->count;
    bs_unlock(bh);
    return reply_integer(cnt);
}

/* ============================================================
 *  §13  BRANK  key score
 *  [rdlock] 스냅샷 후 lock-free 이진 탐색
 * ============================================================ */
s_replyObject *cmd_brank(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3) return reply_error(SHM_ERR_ARGC, "usage: BRANK key score");

    BSetHeader *bh = core_bset_get(h, args[1]->ptr, args[1]->len);
    if (!bh) return reply_nil();

    char    *ep    = NULL;
    uint64_t score = strtoull(args[2]->ptr, &ep, 10);
    if (ep == args[2]->ptr) return reply_error(SHM_ERR_PARSE, "score 변환 실패");

    bs_rdlock(bh);
    uint64_t snap_off   = bh->array_offset;
    uint64_t snap_count = bh->count;
    bs_unlock(bh);

    BSetEntry *arr = (BSetEntry *)OFF2PTR(h, snap_off);
    uint64_t   pos = bs_lower_bound(arr, snap_count, score);
    int found = (pos < snap_count && arr[pos].score == score);
    return found ? reply_integer((int64_t)pos) : reply_nil();
}

/* ============================================================
 *  §14  BCOUNT  key min max
 *  [rdlock] 스냅샷 후 lock-free 이진 탐색
 * ============================================================ */
s_replyObject *cmd_bcount(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 4) return reply_error(SHM_ERR_ARGC, "usage: BCOUNT key min max");

    uint64_t mn = strtoull(args[2]->ptr, NULL, 10);
    uint64_t mx = strtoull(args[3]->ptr, NULL, 10);
    if (mn > mx) return reply_integer(0);

    BSetHeader *bh = core_bset_get(h, args[1]->ptr, args[1]->len);
    if (!bh) return reply_integer(0);

    bs_rdlock(bh);
    uint64_t snap_off   = bh->array_offset;
    uint64_t snap_count = bh->count;
    bs_unlock(bh);

    BSetEntry *arr = (BSetEntry *)OFF2PTR(h, snap_off);
    uint64_t   lo  = bs_lower_bound(arr, snap_count, mn);
    uint64_t   hi  = bs_upper_bound(arr, snap_count, mx);
    return reply_integer((int64_t)(hi >= lo ? hi - lo : 0));
}

/* ============================================================
 *  §15  BPOPMIN / BPOPMAX  key [count]
 *  [wrlock]
 * ============================================================ */
static s_replyObject *bpop_impl(MRedisHandle *h, string_t *args[],
                                 uint32_t argc, int from_min)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC,
        from_min ? "usage: BPOPMIN key [count]" : "usage: BPOPMAX key [count]");

    int64_t count = 1;
    if (argc >= 3) {
        count = strtoll(args[2]->ptr, NULL, 10);
        if (count <= 0) return reply_error(SHM_ERR_INVAL, "count > 0 필요");
    }

    BSetHeader *bh = core_bset_get(h, args[1]->ptr, args[1]->len);
    if (!bh) return reply_array(0);

    bs_wrlock(bh);
    if (count > (int64_t)bh->count) count = (int64_t)bh->count;

    s_replyObject *arr = reply_array((size_t)(count * 2));
    if (!arr) { bs_unlock(bh); return reply_error(SHM_ERR_NOMEM, mredis_strerror(SHM_ERR_NOMEM)); }

    BSetEntry *earr = bs_arr(h, bh);
    for (int64_t i = 0; i < count; i++) {
        uint64_t idx = from_min ? 0 : (bh->count - 1);
        char sbuf[24];
        int  sl = snprintf(sbuf, sizeof(sbuf), "%llu",
                           (unsigned long long)earr[idx].score);
        reply_array_append(arr, reply_string(sbuf, (size_t)(sl > 0 ? sl : 0)));
        const char *vp = earr[idx].vlen > 0
                         ? (const char *)OFF2PTR(h, earr[idx].offset) : "";
        reply_array_append(arr, reply_string(vp, earr[idx].vlen));

        heap_free(h, earr[idx].offset);
        if (from_min && bh->count > 1)
            memmove(&earr[0], &earr[1],
                    (bh->count - 1) * sizeof(BSetEntry));
        bh->count--;
    }
    bs_shrink_if_needed(h, bh);
    bs_unlock(bh);
    return arr;
}
s_replyObject *cmd_bpopmin(MRedisHandle *h, string_t *args[], uint32_t argc)
    { return bpop_impl(h, args, argc, 1); }
s_replyObject *cmd_bpopmax(MRedisHandle *h, string_t *args[], uint32_t argc)
    { return bpop_impl(h, args, argc, 0); }

/* ============================================================
 *  §16  BDROP  key
 *  [wrlock after bucket unlock]
 * ============================================================ */
s_replyObject *cmd_bdrop(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: BDROP key");
    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;

    uint32_t     idx = mredis_hash(key, klen)%((MRedisHeader*)(h->base))->hash_table_size;
    BucketEntry *bk  = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t prev   = OFFSET_NULL;
    uint64_t ne_off = bucket_find_locked(h, bk, key, klen, ENTRY_BSET, &prev);
    if (ne_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return reply_error(SHM_ERR_NOT_FOUND, mredis_strerror(SHM_ERR_NOT_FOUND));
    }
    NameEntry  *ne    = (NameEntry *)OFF2PTR(h, ne_off);
    uint64_t    bh_off = ne->data_offset;
    BSetHeader *bh     = (BSetHeader *)OFF2PTR(h, bh_off);

    if (prev == OFFSET_NULL) bk->head_offset = ne->next_offset;
    else ((NameEntry *)OFF2PTR(h, prev))->next_offset = ne->next_offset;
    nameentry_free(h, ne_off);
    pthread_mutex_unlock(&bk->mutex);

    bs_wrlock(bh);
    BSetEntry *arr = bs_arr(h, bh);
    for (uint64_t i = 0; i < bh->count; i++)
        heap_free(h, arr[i].offset);
    heap_free(h, bh->array_offset);
    bs_unlock(bh);
    pthread_rwlock_destroy(&bh->rwlock);
    heap_free(h, bh_off);
    return reply_ok();
}

/* ── ENTRY_BSET drop ────────────────────────────────────────
 *  ① bucket lock → NameEntry 제거 → bucket unlock
 *  ② BSetHeader.mutex lock → 모든 BSetEntry value 해제 → unlock
 *  ③ BSetHeader + 배열 힙 해제
 * ─────────────────────────────────────────────────────────── */
int drop_bset(MRedisHandle *h, const void *key, uint32_t klen)
{
    uint32_t     idx = mredis_hash(key, klen)%((MRedisHeader*)(h->base))->hash_table_size;
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

    pthread_rwlock_wrlock(&bh->rwlock);
    BSetEntry *arr = (BSetEntry *)OFF2PTR(h, bh->array_offset);
    for (uint64_t i = 0; i < bh->count; i++)
        heap_free(h, arr[i].offset);
    heap_free(h, bh->array_offset);
    pthread_rwlock_unlock(&bh->rwlock);
    pthread_rwlock_destroy(&bh->rwlock);
    heap_free(h, bh_off);

    LOG_TRACE("DEL(BSET): '%.*s'", klen, (const char *)key);
    return SHM_OK;
}
