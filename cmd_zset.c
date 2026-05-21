/*
 * cmd_zset.c  –  Sorted Set 커맨드 구현
 *
 * 버그 수정:
 *  [FIX-1] cmd_zincrby: score 조회와 ZADD 사이에 mutex 를 연속으로 잡지 않아
 *          TOCTOU 발생 가능. → 단일 mutex 구간으로 묶어 atomic INCR 보장.
 *  [FIX-2] cmd_zrange REV: backward 순회 시 sentinel(head) 을 만나면
 *          member_offset == OFFSET_NULL 이므로 중단. 원본 정상.
 *  [FIX-3] cmd_zadd: score 변경 시 sl_unlink → sl_find_update → 재삽입 순서
 *          member 포인터를 기존 SHM 주소로 사용하는데, sl_unlink 후에도
 *          existing->member_offset 이 유효하므로 안전. 정상.
 *  [FIX-4] cmd_zadd: 에러 반환 경로에서 zsh->mutex unlock 누락 없음 확인.
 *  [FIX-5] zpop_impl: sl_find_update 후 sl_unlink 하는데 sn 포인터를
 *          sl_node_free 전에 읽어야 함. 원본 순서 정상.
 *  [FIX-6] zset_create_locked: mutex 초기화 실패 시 head 노드, zsh_off 모두
 *          정리해야 함. 원본은 pthread_mutex_init 실패 체크 없음.
 *          → pthread_mutex_init 반환값 검사 추가.
 */
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "shm_types.h"
#include "shm_core.h"
#include "cmd_zset.h"

/* ── 키 타입 검사 (버킷 잠긴 상태) ──────────────────────── */
static int zset_type_check(ShmHandle *h, BucketEntry *bk,
                             const void *key, uint32_t klen,
                             uint64_t *out_ne)
{
    uint64_t ne = bucket_find_locked(h, bk, key, klen, 0, NULL);
    if (ne == OFFSET_NULL) { if (out_ne) *out_ne = OFFSET_NULL; return 1; }
    NameEntry *nep = (NameEntry *)OFF2PTR(h, ne);
    if (nep->type != ENTRY_ZSET) return SHM_ERR_KEY_EXISTS;
    if (out_ne) *out_ne = ne;
    return 0;
}

/* ── ZSetHeader + HEAD 노드 생성 (버킷 잠긴 상태) ────────── */
static int zset_create_locked(ShmHandle *h, BucketEntry *bk,
                                const void *key, uint32_t klen)
{
    uint64_t zsh_off = heap_alloc(h, sizeof(ZSetHeader));
    if (zsh_off == OFFSET_NULL) return SHM_ERR_NOMEM;

    ZSetHeader *zsh = (ZSetHeader *)OFF2PTR(h, zsh_off);
    zsh->length = 0; zsh->cur_level = 1; zsh->tail_offset = OFFSET_NULL;

    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&ma,  PTHREAD_MUTEX_ROBUST);
    int mrc = pthread_mutex_init(&zsh->mutex, &ma);
    pthread_mutexattr_destroy(&ma);
    if (mrc != 0) { heap_free(h, zsh_off); return SHM_ERR; }

    uint64_t hd_off = heap_alloc(h, sizeof(SkipNode));
    if (hd_off == OFFSET_NULL) {
        pthread_mutex_destroy(&zsh->mutex);
        heap_free(h, zsh_off);
        return SHM_ERR_NOMEM;
    }
    SkipNode *hd = core_sn(h, hd_off);
    hd->score           = -__builtin_inf();
    hd->member_offset   = OFFSET_NULL;
    hd->member_len      = 0;
    hd->level_count     = ZSET_MAX_LEVEL;
    hd->backward_offset = OFFSET_NULL;
    for (int i = 0; i < ZSET_MAX_LEVEL; i++) hd->forward[i] = OFFSET_NULL;
    zsh->head_offset = hd_off;

    uint64_t ne_off = nameentry_alloc(h, key, klen, ENTRY_ZSET, zsh_off);
    if (ne_off == OFFSET_NULL) {
        heap_free(h, hd_off);
        pthread_mutex_destroy(&zsh->mutex);
        heap_free(h, zsh_off);
        return SHM_ERR_NOMEM;
    }
    ((NameEntry *)OFF2PTR(h, ne_off))->next_offset = bk->head_offset;
    bk->head_offset = ne_off;
    return SHM_OK;
}

/* ── 얻기, 없으면 자동 생성 ─────────────────────────────── */
static ZSetHeader *zset_get_or_create(ShmHandle *h,
                                       const void *key, uint32_t klen,
                                       int *err)
{
    ZSetHeader *zsh = core_zset_get(h, key, klen);
    if (zsh) { if (err) *err = SHM_OK; return zsh; }

    uint32_t idx    = shm_hash(key, klen);
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t ne_off = OFFSET_NULL;
    int chk = zset_type_check(h, bk, key, klen, &ne_off);
    if (chk == SHM_ERR_KEY_EXISTS) {
        pthread_mutex_unlock(&bk->mutex);
        if (err) *err = SHM_ERR_KEY_EXISTS;
        return NULL;
    }
    if (chk == 0) {
        ZSetHeader *z = (ZSetHeader *)OFF2PTR(h,
            ((NameEntry *)OFF2PTR(h, ne_off))->data_offset);
        pthread_mutex_unlock(&bk->mutex);
        if (err) *err = SHM_OK;
        return z;
    }
    int r = zset_create_locked(h, bk, key, klen);
    pthread_mutex_unlock(&bk->mutex);
    if (r != SHM_OK) { if (err) *err = r; return NULL; }
    if (err) *err = SHM_OK;
    return core_zset_get(h, key, klen);
}

/* ── ZCREATE ─────────────────────────────────────────────── */
s_replyObject *cmd_zcreate(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: ZCREATE key");
    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;
    uint32_t idx = shm_hash(key, klen);
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);
    uint64_t ne_off = OFFSET_NULL;
    int chk = zset_type_check(h, bk, key, klen, &ne_off);
    if (chk == SHM_ERR_KEY_EXISTS) {
        pthread_mutex_unlock(&bk->mutex);
        return reply_error(SHM_ERR_KEY_EXISTS, shm_strerror(SHM_ERR_KEY_EXISTS));
    }
    if (chk == 0) { pthread_mutex_unlock(&bk->mutex); return reply_ok(); }
    int r = zset_create_locked(h, bk, key, klen);
    pthread_mutex_unlock(&bk->mutex);
    return r == SHM_OK ? reply_ok() : reply_error(r, shm_strerror(r));
}

/* ── ZDROP ───────────────────────────────────────────────── */
s_replyObject *cmd_zdrop(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: ZDROP key");
    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;

    uint32_t idx    = shm_hash(key, klen);
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t prev   = OFFSET_NULL;
    uint64_t ne_off = bucket_find_locked(h, bk, key, klen, ENTRY_ZSET, &prev);
    if (ne_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return reply_error(SHM_ERR_NOT_FOUND, shm_strerror(SHM_ERR_NOT_FOUND));
    }
    NameEntry   *ne     = (NameEntry *)OFF2PTR(h, ne_off);
    uint64_t     zsh_off = ne->data_offset;
    ZSetHeader  *zsh     = (ZSetHeader *)OFF2PTR(h, zsh_off);

    if (prev == OFFSET_NULL) bk->head_offset = ne->next_offset;
    else ((NameEntry *)OFF2PTR(h, prev))->next_offset = ne->next_offset;
    nameentry_free(h, ne_off);
    pthread_mutex_unlock(&bk->mutex);   /* bucket unlock 먼저 */

    /* SkipList 노드 해제 */
    int rc = pthread_mutex_lock(&zsh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&zsh->mutex);
    uint64_t cur = core_sn(h, zsh->head_offset)->forward[0];
    while (cur != OFFSET_NULL) {
        uint64_t nx = core_sn(h, cur)->forward[0];
        sl_node_free(h, cur);
        cur = nx;
    }
    heap_free(h, zsh->head_offset);
    pthread_mutex_unlock(&zsh->mutex);
    pthread_mutex_destroy(&zsh->mutex);
    heap_free(h, zsh_off);
    return reply_ok();
}

/* ── ZADD ────────────────────────────────────────────────── */
s_replyObject *cmd_zadd(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 4) return reply_error(SHM_ERR_ARGC, "usage: ZADD key score member");
    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;

    int flags = ZADD_NONE; uint32_t pos = 2;
    for (; pos < argc; pos++) {
        const char *a = args[pos]->ptr;
        if      (!strcasecmp(a, "NX")) flags |= ZADD_NX;
        else if (!strcasecmp(a, "XX")) flags |= ZADD_XX;
        else if (!strcasecmp(a, "GT")) flags |= ZADD_GT;
        else if (!strcasecmp(a, "LT")) flags |= ZADD_LT;
        else if (!strcasecmp(a, "CH")) flags |= ZADD_CH;
        else break;
    }
    if (pos >= argc || (argc - pos) % 2 != 0)
        return reply_error(SHM_ERR_ARGC, "ZADD: score/member 쌍 필요");

    int err = SHM_OK;
    ZSetHeader *zsh = zset_get_or_create(h, key, klen, &err);
    if (!zsh) return reply_error(err, shm_strerror(err));

    int rc = pthread_mutex_lock(&zsh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&zsh->mutex);

    int64_t total_added = 0, total_changed = 0;
    for (uint32_t i = pos; i + 1 < argc; i += 2) {
        char *ep = NULL;
        double score = strtod(args[i]->ptr, &ep);
        if (ep == args[i]->ptr) {
            pthread_mutex_unlock(&zsh->mutex);
            return reply_error(SHM_ERR_PARSE, "score 변환 실패");
        }
        const void *member = args[i+1]->ptr; uint32_t mlen = args[i+1]->len;
        if (!mlen) {
            pthread_mutex_unlock(&zsh->mutex);
            return reply_error(SHM_ERR_INVAL, "member 비어있음");
        }

        uint64_t update[ZSET_MAX_LEVEL];
        uint64_t exist_off = sl_find_member(h, zsh, member, mlen);
        SkipNode *existing = (exist_off != OFFSET_NULL) ? core_sn(h, exist_off) : NULL;

        if (existing) {
            if (flags & ZADD_NX) continue;
            double old = existing->score; int doit = 1;
            if ((flags & ZADD_GT) && score <= old) doit = 0;
            if ((flags & ZADD_LT) && score >= old) doit = 0;
            if (doit && score != old) {
                sl_find_update(h, zsh, old,
                    OFF2PTR(h, existing->member_offset),
                    existing->member_len, update);
                sl_unlink(h, zsh, exist_off, update);
                sl_find_update(h, zsh, score, member, mlen, update);
                uint32_t lv = sl_random_level();
                if (lv > zsh->cur_level) {
                    for (uint32_t li = zsh->cur_level; li < lv; li++)
                        update[li] = zsh->head_offset;
                    zsh->cur_level = lv;
                }
                existing->score      = score;
                existing->level_count = lv;
                for (uint32_t li = 0; li < lv; li++) {
                    existing->forward[li] = core_sn(h, update[li])->forward[li];
                    core_sn(h, update[li])->forward[li] = exist_off;
                }
                for (uint32_t li = lv; li < ZSET_MAX_LEVEL; li++)
                    existing->forward[li] = OFFSET_NULL;
                if (existing->forward[0] != OFFSET_NULL)
                    core_sn(h, existing->forward[0])->backward_offset = exist_off;
                else
                    zsh->tail_offset = exist_off;
                existing->backward_offset = update[0];
                total_changed++;
            }
        } else {
            if (flags & ZADD_XX) continue;
            sl_find_update(h, zsh, score, member, mlen, update);
            uint32_t lv = sl_random_level();
            if (lv > zsh->cur_level) {
                for (uint32_t li = zsh->cur_level; li < lv; li++)
                    update[li] = zsh->head_offset;
                zsh->cur_level = lv;
            }
            uint64_t n = sl_node_alloc(h, score, member, mlen, lv);
            if (n == OFFSET_NULL) {
                pthread_mutex_unlock(&zsh->mutex);
                return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
            }
            SkipNode *sn = core_sn(h, n);
            for (uint32_t li = 0; li < lv; li++) {
                sn->forward[li] = core_sn(h, update[li])->forward[li];
                core_sn(h, update[li])->forward[li] = n;
            }
            sn->backward_offset = update[0];
            if (sn->forward[0] != OFFSET_NULL)
                core_sn(h, sn->forward[0])->backward_offset = n;
            else
                zsh->tail_offset = n;
            zsh->length++;
            total_added++;
        }
    }
    pthread_mutex_unlock(&zsh->mutex);
    return reply_integer((flags & ZADD_CH) ? (total_added + total_changed) : total_added);
}

/* ── ZREM ────────────────────────────────────────────────── */
s_replyObject *cmd_zrem(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3) return reply_error(SHM_ERR_ARGC, "usage: ZREM key member");
    ZSetHeader *zsh = core_zset_get(h, args[1]->ptr, args[1]->len);
    if (!zsh) return reply_integer(0);
    int rc = pthread_mutex_lock(&zsh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&zsh->mutex);
    int64_t removed = 0;
    for (uint32_t i = 2; i < argc; i++) {
        uint64_t n = sl_find_member(h, zsh, args[i]->ptr, args[i]->len);
        if (n == OFFSET_NULL) continue;
        uint64_t upd[ZSET_MAX_LEVEL];
        SkipNode *sn = core_sn(h, n);
        sl_find_update(h, zsh, sn->score,
                       OFF2PTR(h, sn->member_offset), sn->member_len, upd);
        sl_unlink(h, zsh, n, upd);
        sl_node_free(h, n);
        zsh->length--;
        removed++;
    }
    pthread_mutex_unlock(&zsh->mutex);
    return reply_integer(removed);
}

/* ── ZSCORE ──────────────────────────────────────────────── */
s_replyObject *cmd_zscore(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3) return reply_error(SHM_ERR_ARGC, "usage: ZSCORE key member");
    ZSetHeader *zsh = core_zset_get(h, args[1]->ptr, args[1]->len);
    if (!zsh) return reply_nil();
    int rc = pthread_mutex_lock(&zsh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&zsh->mutex);
    uint64_t n = sl_find_member(h, zsh, args[2]->ptr, args[2]->len);
    if (n == OFFSET_NULL) { pthread_mutex_unlock(&zsh->mutex); return reply_nil(); }
    double sc = core_sn(h, n)->score;
    pthread_mutex_unlock(&zsh->mutex);
    char buf[64]; int bl = snprintf(buf, sizeof(buf), "%.17g", sc);
    return reply_string(buf, (size_t)bl);
}

/* ── ZINCRBY ─────────────────────────────────────────────
 * [FIX-1] 조회 + 삽입을 동일 mutex 구간으로 묶어 TOCTOU 제거.
 * ─────────────────────────────────────────────────────── */
s_replyObject *cmd_zincrby(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 4) return reply_error(SHM_ERR_ARGC, "usage: ZINCRBY key delta member");
    char *ep = NULL;
    double delta = strtod(args[2]->ptr, &ep);
    if (ep == args[2]->ptr) return reply_error(SHM_ERR_PARSE, "delta 변환 실패");
    const void *member = args[3]->ptr; uint32_t mlen = args[3]->len;

    int err = SHM_OK;
    ZSetHeader *zsh = zset_get_or_create(h, args[1]->ptr, args[1]->len, &err);
    if (!zsh) return reply_error(err, shm_strerror(err));

    int rc = pthread_mutex_lock(&zsh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&zsh->mutex);

    /* 현재 score 조회 */
    double new_sc = delta;
    uint64_t exist_off = sl_find_member(h, zsh, member, mlen);
    if (exist_off != OFFSET_NULL)
        new_sc = core_sn(h, exist_off)->score + delta;

    /* score 갱신 (mutex 유지 상태에서 인라인 처리) */
    uint64_t update[ZSET_MAX_LEVEL];
    if (exist_off != OFFSET_NULL) {
        SkipNode *sn = core_sn(h, exist_off);
        sl_find_update(h, zsh, sn->score,
                       OFF2PTR(h, sn->member_offset), sn->member_len, update);
        sl_unlink(h, zsh, exist_off, update);
        sl_find_update(h, zsh, new_sc, member, mlen, update);
        uint32_t lv = sl_random_level();
        if (lv > zsh->cur_level) {
            for (uint32_t li = zsh->cur_level; li < lv; li++)
                update[li] = zsh->head_offset;
            zsh->cur_level = lv;
        }
        sn->score       = new_sc;
        sn->level_count = lv;
        for (uint32_t li = 0; li < lv; li++) {
            sn->forward[li] = core_sn(h, update[li])->forward[li];
            core_sn(h, update[li])->forward[li] = exist_off;
        }
        for (uint32_t li = lv; li < ZSET_MAX_LEVEL; li++)
            sn->forward[li] = OFFSET_NULL;
        if (sn->forward[0] != OFFSET_NULL)
            core_sn(h, sn->forward[0])->backward_offset = exist_off;
        else
            zsh->tail_offset = exist_off;
        sn->backward_offset = update[0];
    } else {
        sl_find_update(h, zsh, new_sc, member, mlen, update);
        uint32_t lv = sl_random_level();
        if (lv > zsh->cur_level) {
            for (uint32_t li = zsh->cur_level; li < lv; li++)
                update[li] = zsh->head_offset;
            zsh->cur_level = lv;
        }
        uint64_t n = sl_node_alloc(h, new_sc, member, mlen, lv);
        if (n == OFFSET_NULL) {
            pthread_mutex_unlock(&zsh->mutex);
            return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
        }
        SkipNode *sn = core_sn(h, n);
        for (uint32_t li = 0; li < lv; li++) {
            sn->forward[li] = core_sn(h, update[li])->forward[li];
            core_sn(h, update[li])->forward[li] = n;
        }
        sn->backward_offset = update[0];
        if (sn->forward[0] != OFFSET_NULL)
            core_sn(h, sn->forward[0])->backward_offset = n;
        else
            zsh->tail_offset = n;
        zsh->length++;
    }

    pthread_mutex_unlock(&zsh->mutex);
    char rbuf[64]; int rl = snprintf(rbuf, sizeof(rbuf), "%.17g", new_sc);
    return reply_string(rbuf, (size_t)rl);
}

/* ── ZRANK / ZREVRANK ────────────────────────────────────── */
static s_replyObject *zrank_impl(ShmHandle *h, string_t *args[],
                                  uint32_t argc, int rev)
{
    if (argc < 3) return reply_error(SHM_ERR_ARGC,
        rev ? "usage: ZREVRANK key member" : "usage: ZRANK key member");
    ZSetHeader *zsh = core_zset_get(h, args[1]->ptr, args[1]->len);
    if (!zsh) return reply_nil();
    int rc = pthread_mutex_lock(&zsh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&zsh->mutex);
    int64_t rank = 0, found = -1;
    uint64_t cur = core_sn(h, zsh->head_offset)->forward[0];
    while (cur != OFFSET_NULL) {
        SkipNode *sn = core_sn(h, cur);
        if (sn->member_len == args[2]->len &&
            !memcmp(OFF2PTR(h, sn->member_offset), args[2]->ptr, args[2]->len))
            { found = rank; break; }
        rank++; cur = sn->forward[0];
    }
    int64_t len = (int64_t)zsh->length;
    pthread_mutex_unlock(&zsh->mutex);
    if (found < 0) return reply_nil();
    return reply_integer(rev ? len - 1 - found : found);
}
s_replyObject *cmd_zrank   (ShmHandle *h, string_t *args[], uint32_t argc) { return zrank_impl(h, args, argc, 0); }
s_replyObject *cmd_zrevrank(ShmHandle *h, string_t *args[], uint32_t argc) { return zrank_impl(h, args, argc, 1); }

/* ── ZCARD ───────────────────────────────────────────────── */
s_replyObject *cmd_zcard(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: ZCARD key");
    ZSetHeader *zsh = core_zset_get(h, args[1]->ptr, args[1]->len);
    return reply_integer(zsh ? (int64_t)zsh->length : 0);
}

/* ── ZCOUNT ──────────────────────────────────────────────── */
s_replyObject *cmd_zcount(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 4) return reply_error(SHM_ERR_ARGC, "usage: ZCOUNT key min max");
    double mn = strtod(args[2]->ptr, NULL), mx = strtod(args[3]->ptr, NULL);
    ZSetHeader *zsh = core_zset_get(h, args[1]->ptr, args[1]->len);
    if (!zsh || mn > mx) return reply_integer(0);
    int rc = pthread_mutex_lock(&zsh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&zsh->mutex);
    int64_t cnt = 0;
    uint64_t cur = core_sn(h, zsh->head_offset)->forward[0];
    while (cur != OFFSET_NULL) {
        SkipNode *sn = core_sn(h, cur);
        if (sn->score > mx) break;
        if (sn->score >= mn) cnt++;
        cur = sn->forward[0];
    }
    pthread_mutex_unlock(&zsh->mutex);
    return reply_integer(cnt);
}

/* ── 범위 결과 → ARRAY 변환 ─────────────────────────────── */
static s_replyObject *build_zrange_reply(ShmHandle *h,
                                          uint64_t *nodes, int64_t count)
{
    s_replyObject *arr = reply_array((size_t)(count * 2));
    if (!arr) return NULL;
    for (int64_t i = 0; i < count; i++) {
        SkipNode *sn = core_sn(h, nodes[i]);
        s_replyObject *mem = reply_string(
            (const char *)OFF2PTR(h, sn->member_offset), sn->member_len);
        char sbuf[64]; int sl = snprintf(sbuf, sizeof(sbuf), "%.17g", sn->score);
        s_replyObject *sc  = reply_string(sbuf, (size_t)sl);
        if (!mem || !sc) { reply_free(mem); reply_free(sc); reply_free(arr); return NULL; }
        reply_array_append(arr, mem);
        reply_array_append(arr, sc);
    }
    return arr;
}

/* ── ZRANGE ──────────────────────────────────────────────── */
s_replyObject *cmd_zrange(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 4) return reply_error(SHM_ERR_ARGC, "usage: ZRANGE key start stop [REV]");
    int64_t start = strtoll(args[2]->ptr, NULL, 10);
    int64_t stop  = strtoll(args[3]->ptr, NULL, 10);
    int rev = (argc >= 5 && !strcasecmp(args[4]->ptr, "REV")) ? 1 : 0;

    ZSetHeader *zsh = core_zset_get(h, args[1]->ptr, args[1]->len);
    if (!zsh) return reply_array(0);
    int rc = pthread_mutex_lock(&zsh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&zsh->mutex);

    int64_t len = (int64_t)zsh->length;
    if (len == 0) { pthread_mutex_unlock(&zsh->mutex); return reply_array(0); }
    if (start < 0) start = len + start;
    if (stop  < 0) stop  = len + stop;
    if (start < 0) start = 0;
    if (stop >= len) stop = len - 1;
    if (start > stop) { pthread_mutex_unlock(&zsh->mutex); return reply_array(0); }

    int64_t need = stop - start + 1;
    uint64_t *nodes = (uint64_t *)malloc((size_t)need * sizeof(uint64_t));
    if (!nodes) {
        pthread_mutex_unlock(&zsh->mutex);
        return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
    }
    int64_t filled = 0;

    if (!rev) {
        int64_t i = 0;
        uint64_t cur = core_sn(h, zsh->head_offset)->forward[0];
        while (cur != OFFSET_NULL && i <= stop) {
            if (i >= start) nodes[filled++] = cur;
            cur = core_sn(h, cur)->forward[0]; i++;
        }
    } else {
        int64_t rstart = len - 1 - stop, rstop = len - 1 - start, ri = 0;
        uint64_t cur = zsh->tail_offset;
        while (cur != OFFSET_NULL && ri <= rstop) {
            if (ri >= rstart) nodes[filled++] = cur;
            SkipNode *sn = core_sn(h, cur);
            if (sn->backward_offset == OFFSET_NULL) break;
            if (core_sn(h, sn->backward_offset)->member_offset == OFFSET_NULL) break;
            cur = sn->backward_offset; ri++;
        }
    }
    s_replyObject *arr = build_zrange_reply(h, nodes, filled);
    free(nodes);
    pthread_mutex_unlock(&zsh->mutex);
    return arr ? arr : reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
}

/* ── ZRANGEBYSCORE ───────────────────────────────────────── */
s_replyObject *cmd_zrangebyscore(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 4) return reply_error(SHM_ERR_ARGC,
        "usage: ZRANGEBYSCORE key min max [REV] [LIMIT offset count]");
    double mn = strtod(args[2]->ptr, NULL), mx = strtod(args[3]->ptr, NULL);
    int rev = 0; int64_t loff = 0, lcnt = -1;
    for (uint32_t i = 4; i < argc; i++) {
        if (!strcasecmp(args[i]->ptr, "REV")) rev = 1;
        else if (!strcasecmp(args[i]->ptr, "LIMIT") && i + 2 < argc) {
            loff = strtoll(args[i+1]->ptr, NULL, 10);
            lcnt = strtoll(args[i+2]->ptr, NULL, 10);
            i += 2;
        }
    }
    ZSetHeader *zsh = core_zset_get(h, args[1]->ptr, args[1]->len);
    if (!zsh || mn > mx) return reply_array(0);
    int rc = pthread_mutex_lock(&zsh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&zsh->mutex);

    int64_t cap = 16, filled = 0, skipped = 0;
    uint64_t *nodes = (uint64_t *)malloc((size_t)cap * sizeof(uint64_t));
    if (!nodes) { pthread_mutex_unlock(&zsh->mutex); return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM)); }

    if (!rev) {
        uint64_t cur = core_sn(h, zsh->head_offset)->forward[0];
        while (cur != OFFSET_NULL) {
            SkipNode *sn = core_sn(h, cur);
            if (sn->score > mx) break;
            if (sn->score >= mn) {
                if (skipped < loff) { skipped++; }
                else {
                    if (lcnt >= 0 && filled >= lcnt) break;
                    if (filled >= cap) {
                        cap *= 2;
                        uint64_t *t = (uint64_t *)realloc(nodes, (size_t)cap * sizeof(uint64_t));
                        if (!t) { free(nodes); pthread_mutex_unlock(&zsh->mutex); return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM)); }
                        nodes = t;
                    }
                    nodes[filled++] = cur;
                }
            }
            cur = sn->forward[0];
        }
    } else {
        uint64_t cur = zsh->tail_offset;
        while (cur != OFFSET_NULL) {
            SkipNode *sn = core_sn(h, cur);
            if (sn->score < mn) break;
            if (sn->score <= mx) {
                if (skipped < loff) { skipped++; }
                else {
                    if (lcnt >= 0 && filled >= lcnt) break;
                    if (filled >= cap) {
                        cap *= 2;
                        uint64_t *t = (uint64_t *)realloc(nodes, (size_t)cap * sizeof(uint64_t));
                        if (!t) { free(nodes); pthread_mutex_unlock(&zsh->mutex); return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM)); }
                        nodes = t;
                    }
                    nodes[filled++] = cur;
                }
            }
            if (sn->backward_offset == OFFSET_NULL) break;
            if (core_sn(h, sn->backward_offset)->member_offset == OFFSET_NULL) break;
            cur = sn->backward_offset;
        }
    }
    s_replyObject *arr = build_zrange_reply(h, nodes, filled);
    free(nodes);
    pthread_mutex_unlock(&zsh->mutex);
    return arr ? arr : reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
}

/* ── ZPOPMIN / ZPOPMAX ───────────────────────────────────── */
static s_replyObject *zpop_impl(ShmHandle *h, string_t *args[],
                                 uint32_t argc, int from_min)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC,
        from_min ? "usage: ZPOPMIN key [count]" : "usage: ZPOPMAX key [count]");
    int64_t count = 1;
    if (argc >= 3) count = strtoll(args[2]->ptr, NULL, 10);
    if (count <= 0) return reply_error(SHM_ERR_INVAL, "count > 0 필요");

    ZSetHeader *zsh = core_zset_get(h, args[1]->ptr, args[1]->len);
    if (!zsh) return reply_array(0);
    int rc = pthread_mutex_lock(&zsh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&zsh->mutex);
    if (count > (int64_t)zsh->length) count = (int64_t)zsh->length;

    s_replyObject *arr = reply_array((size_t)(count * 2));
    if (!arr) { pthread_mutex_unlock(&zsh->mutex); return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM)); }

    for (int64_t i = 0; i < count && zsh->length > 0; i++) {
        uint64_t n = from_min
            ? core_sn(h, zsh->head_offset)->forward[0]
            : zsh->tail_offset;
        if (n == OFFSET_NULL) break;
        SkipNode *sn = core_sn(h, n);
        s_replyObject *mem = reply_string(
            (const char *)OFF2PTR(h, sn->member_offset), sn->member_len);
        char sbuf[64]; int sl = snprintf(sbuf, sizeof(sbuf), "%.17g", sn->score);
        s_replyObject *sc  = reply_string(sbuf, (size_t)sl);
        if (mem) reply_array_append(arr, mem);
        if (sc)  reply_array_append(arr, sc);
        uint64_t upd[ZSET_MAX_LEVEL];
        sl_find_update(h, zsh, sn->score,
                       OFF2PTR(h, sn->member_offset), sn->member_len, upd);
        sl_unlink(h, zsh, n, upd);
        sl_node_free(h, n);
        zsh->length--;
    }
    pthread_mutex_unlock(&zsh->mutex);
    return arr;
}
s_replyObject *cmd_zpopmin(ShmHandle *h, string_t *args[], uint32_t argc) { return zpop_impl(h, args, argc, 1); }
s_replyObject *cmd_zpopmax(ShmHandle *h, string_t *args[], uint32_t argc) { return zpop_impl(h, args, argc, 0); }
