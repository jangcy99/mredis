/*
 * cmd_hash.c  –  Hash Map 커맨드 구현 (s_replyObject 반환)
 *
 *  HashHeader 는 힙에 아래 레이아웃으로 저장된다.
 *  [HashHeader][uint64_t field_buckets[n_buckets]]
 *
 *  FieldEntry 체인은 field_buckets[FNV(field) % n_buckets] 에서 시작한다.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "shm_types.h"
#include "shm_core.h"
#include "cmd_hash.h"

/* Field 이름 Interning (재사용) */
static uint64_t get_or_intern_field(ShmHandle *h, const void *field, uint32_t flen)
{
    FieldPoolHeader *fp = get_field_pool(h);
    if (!fp) return OFFSET_NULL;

    int rc = pthread_mutex_lock(&fp->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&fp->mutex);

    uint32_t bi = shm_field_hash(field, flen, 256);
    uint64_t cur = fp->field_buckets[bi];

    while (cur != OFFSET_NULL) {
        FieldString *fs = (FieldString *)OFF2PTR(h, cur);
        if (fs->str_len == flen &&
            memcmp(OFF2PTR(h, fs->str_offset), field, flen) == 0) {
            fs->ref_count++;
            pthread_mutex_unlock(&fp->mutex);
            return fs->str_offset;
        }
        cur = fs->next;
    }

    /* 신규 Intern */
    uint64_t str_off = heap_alloc(h, flen);
    uint64_t fs_off = heap_alloc(h, sizeof(FieldString));
    if (str_off == OFFSET_NULL || fs_off == OFFSET_NULL) {
        pthread_mutex_unlock(&fp->mutex);
        return OFFSET_NULL;
    }

    memcpy(OFF2PTR(h, str_off), field, flen);

    FieldString *fs = (FieldString *)OFF2PTR(h, fs_off);
    fs->next = fp->field_buckets[bi];
    fs->str_offset = str_off;
    fs->str_len = flen;
    fs->ref_count = 1;

    fp->field_buckets[bi] = fs_off;
    fp->total_fields++;

    pthread_mutex_unlock(&fp->mutex);
    return str_off;
}

static int decrease_field_ref(ShmHandle *h, const void *field, uint32_t flen) {
    FieldPoolHeader *fp = get_field_pool(h);
    if (!fp) return -1;

    int rc = pthread_mutex_lock(&fp->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&fp->mutex);

    uint32_t bi = shm_field_hash(field, flen, 256);
    uint64_t cur = fp->field_buckets[bi];
	uint64_t prev = OFFSET_NULL;
    while (cur != OFFSET_NULL) {
        FieldString *fs = (FieldString *)OFF2PTR(h, cur);
        if (fs->str_len == flen && memcmp(OFF2PTR(h, fs->str_offset), field, flen) == 0) {
            fs->ref_count--;
			if (fs->ref_count <= 0)	{
				// FieldString삭제
				heap_free (h, fs->str_offset);
				if (prev == OFFSET_NULL)	fp->field_buckets[bi] = fs->next;
				else	((FieldString*)OFF2PTR(h,prev))->next = fs->next;
				heap_free (h, cur);
			}
            pthread_mutex_unlock(&fp->mutex);
            return 0;
        }
		prev = cur;
        cur = fs->next;
    }
	LOG_TRACE ("HSET DECREASE NOTFOUND:%.*s", flen, (char*)field);
	pthread_mutex_unlock(&fp->mutex);
	return -1;	// notfound 
}
/* ============================================================
 *  내부 헬퍼
 * ============================================================ */

/* ── 키 타입 검사 (버킷 잠긴 상태) ──────────────────────────
 *   1 = 새 키   0 = 기존 ENTRY_HASH   SHM_ERR_KEY_EXISTS = 다른 타입
 * ─────────────────────────────────────────────────────────── */
static int hash_type_check(ShmHandle *h, BucketEntry *bk,
                             const void *key, uint32_t klen,
                             uint64_t *out_ne)
{
    uint64_t ne = bucket_find_locked(h, bk, key, klen, 0, NULL);
    if (ne == OFFSET_NULL) { if (out_ne) *out_ne = OFFSET_NULL; return 1; }
    NameEntry *nep = (NameEntry *)OFF2PTR(h, ne);
    if (nep->type != ENTRY_HASH) return SHM_ERR_KEY_EXISTS;
    if (out_ne) *out_ne = ne;
    return 0;
}

/* ── HashHeader 전체 크기 (헤더 + 버킷 배열) ─────────────── */
static inline uint64_t hh_alloc_size(uint32_t nb)
{
    return (uint64_t)sizeof(HashHeader) + (uint64_t)nb * sizeof(uint64_t);
}

/* ── HashHeader 신규 생성 ────────────────────────────────── */
static uint64_t hashheader_new(ShmHandle *h, uint32_t nb)
{
    uint64_t off = heap_alloc(h, hh_alloc_size(nb));
    if (off == OFFSET_NULL) return OFFSET_NULL;
    HashHeader *hh = (HashHeader *)OFF2PTR(h, off);
    hh->field_count = 0; hh->n_buckets = nb;
    pthread_mutexattr_t ma; pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&hh->mutex, &ma); pthread_mutexattr_destroy(&ma);
    uint64_t *bkts = hh_field_buckets(hh);
    for (uint32_t i = 0; i < nb; i++) bkts[i] = OFFSET_NULL;
    return off;
}

/* ── field 탐색 (HashHeader 뮤텍스 잠긴 상태) ──────────────
 *  반환: FieldEntry 오프셋 (없으면 OFFSET_NULL)
 * ─────────────────────────────────────────────────────────── */
static uint64_t field_find_locked(ShmHandle *h, HashHeader *hh,
                                   const void *field, uint32_t flen,
                                   uint64_t *out_prev)
{
    uint64_t *bkts = hh_field_buckets(hh);
    uint32_t bi    = shm_field_hash(field, flen, hh->n_buckets);
    uint64_t prev  = OFFSET_NULL, cur = bkts[bi];
    while (cur != OFFSET_NULL) {
        FieldEntry *fe = (FieldEntry *)OFF2PTR(h, cur);
        if (fe->field_len == flen &&
            memcmp(OFF2PTR(h, fe->field_offset), field, flen) == 0) {
            if (out_prev) *out_prev = prev;
            return cur;
        }
        prev = cur;
		cur = fe->next_offset;
    }
    if (out_prev) *out_prev = prev;
    return OFFSET_NULL;
}

/* ── HashHeader 얻기 (없으면 자동 생성) ─────────────────────
 *  err_out 에 SHM_ERR_KEY_EXISTS 등 에러 코드 설정
 * ─────────────────────────────────────────────────────────── */
static HashHeader *hash_get_or_create(ShmHandle *h,
                                       const void *key, uint32_t klen,
                                       int *err_out)
{
    HashHeader *hh = core_hash_get(h, key, klen);
    if (hh) { if (err_out) *err_out = SHM_OK; return hh; }

    uint32_t idx    = shm_hash(key, klen);
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t ne_off = OFFSET_NULL;
    int chk = hash_type_check(h, bk, key, klen, &ne_off);

    if (chk == SHM_ERR_KEY_EXISTS) {
        pthread_mutex_unlock(&bk->mutex);
        if (err_out) *err_out = SHM_ERR_KEY_EXISTS;
        return NULL;
    }
    if (chk == 0) {
        /* 동시 생성 경쟁 – 이미 존재 */
        hh = (HashHeader *)OFF2PTR(h,
             ((NameEntry *)OFF2PTR(h, ne_off))->data_offset);
        pthread_mutex_unlock(&bk->mutex);
        if (err_out) *err_out = SHM_OK;
        return hh;
    }
    /* 신규 생성 */
    uint64_t hh_off = hashheader_new(h, HASH_FIELD_BUCKETS);
    if (hh_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        if (err_out) *err_out = SHM_ERR_NOMEM;
        return NULL;
    }
    uint64_t new_ne = nameentry_alloc(h, key, klen, ENTRY_HASH, hh_off);
    if (new_ne == OFFSET_NULL) {
        /* HashHeader 생성 후 mutex 파괴 필요 */
        HashHeader *tmp = (HashHeader *)OFF2PTR(h, hh_off);
        pthread_mutex_destroy(&tmp->mutex);
        heap_free(h, hh_off);
        pthread_mutex_unlock(&bk->mutex);
        if (err_out) *err_out = SHM_ERR_NOMEM;
        return NULL;
    }
    ((NameEntry *)OFF2PTR(h, new_ne))->next_offset = bk->head_offset;
    bk->head_offset = new_ne;
    pthread_mutex_unlock(&bk->mutex);

    if (err_out) *err_out = SHM_OK;
    return (HashHeader *)OFF2PTR(h, hh_off);
}

/* ── 모든 field 해제 (뮤텍스 잠긴 상태) ─────────────────── */
static void hash_drop_fields_locked(ShmHandle *h, HashHeader *hh)
{
    uint64_t *bkts = hh_field_buckets(hh);
    for (uint32_t i = 0; i < hh->n_buckets; i++) {
        uint64_t cur = bkts[i];
        while (cur != OFFSET_NULL) {
            FieldEntry *fe = (FieldEntry *)OFF2PTR(h, cur);
            uint64_t nxt   = fe->next_offset;
			decrease_field_ref(h, OFF2PTR(h, fe->field_offset), fe->field_len);
            heap_free(h, fe->val_offset);
            heap_free(h, cur);
            cur = nxt;
        }
        bkts[i] = OFFSET_NULL;
    }
    hh->field_count = 0;
}

/* ============================================================
 *  HCREATE
 * ============================================================ */
s_replyObject *cmd_hcreate(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: HCREATE key");
    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;
    if (klen == 0) return reply_error(SHM_ERR_INVAL, "key 비어있음");

    uint32_t idx    = shm_hash(key, klen);
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t ne_off = OFFSET_NULL;
    int chk = hash_type_check(h, bk, key, klen, &ne_off);

    if (chk == SHM_ERR_KEY_EXISTS) {
        pthread_mutex_unlock(&bk->mutex);
        LOG_ERR("HCREATE: '%.*s' 다른 타입 존재", klen, (const char *)key);
        return reply_error(SHM_ERR_KEY_EXISTS, shm_strerror(SHM_ERR_KEY_EXISTS));
    }
    if (chk == 0) {
        pthread_mutex_unlock(&bk->mutex);
        return reply_ok(); /* 이미 존재 */
    }

    uint64_t hh_off = hashheader_new(h, HASH_FIELD_BUCKETS);
    if (hh_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
    }
    uint64_t new_ne = nameentry_alloc(h, key, klen, ENTRY_HASH, hh_off);
    if (new_ne == OFFSET_NULL) {
        HashHeader *tmp = (HashHeader *)OFF2PTR(h, hh_off);
        pthread_mutex_destroy(&tmp->mutex);
        heap_free(h, hh_off);
        pthread_mutex_unlock(&bk->mutex);
        return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
    }
    ((NameEntry *)OFF2PTR(h, new_ne))->next_offset = bk->head_offset;
    bk->head_offset = new_ne;
    pthread_mutex_unlock(&bk->mutex);
    LOG_TRACE("HCREATE: '%.*s' 완료", klen, (const char *)key);
    return reply_ok();
}

/* ============================================================
 *  HDROP
 * ============================================================ */
s_replyObject *cmd_hdrop(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: HDROP key");
    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;

    uint32_t idx    = shm_hash(key, klen);
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t prev   = OFFSET_NULL;
    uint64_t ne_off = bucket_find_locked(h, bk, key, klen, ENTRY_HASH, &prev);
    if (ne_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return reply_error(SHM_ERR_NOT_FOUND, shm_strerror(SHM_ERR_NOT_FOUND));
    }
    NameEntry  *ne    = (NameEntry *)OFF2PTR(h, ne_off);
    uint64_t    hh_off = ne->data_offset;
    HashHeader *hh     = (HashHeader *)OFF2PTR(h, hh_off);

    if (prev == OFFSET_NULL) bk->head_offset = ne->next_offset;
    else ((NameEntry *)OFF2PTR(h, prev))->next_offset = ne->next_offset;
    nameentry_free(h, ne_off);
    pthread_mutex_unlock(&bk->mutex);

    /* 내부 뮤텍스 잠금 후 모든 field 해제 */
    int rc = pthread_mutex_lock(&hh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&hh->mutex);
    hash_drop_fields_locked(h, hh);
    pthread_mutex_unlock(&hh->mutex);
    pthread_mutex_destroy(&hh->mutex);
    heap_free(h, hh_off);
    LOG_TRACE("HDROP: '%.*s' 완료", klen, (const char *)key);
    return reply_ok();
}

/* ============================================================
 *  HSET  key field value [field value …]
 *  반환: INTEGER (새로 추가된 field 수)
 * ============================================================ */
s_replyObject *cmd_hset(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 4 || (argc - 2) % 2 != 0)
        return reply_error(SHM_ERR_ARGC, "usage: HSET key field value [field value …]");

    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;
    if (klen == 0) return reply_error(SHM_ERR_INVAL, "key 비어있음");

    /* 키 타입 충돌 사전 검사 */
    {
        uint32_t idx    = shm_hash(key, klen);
        BucketEntry *bk = core_get_bucket(h, idx);
        bucket_lock(h, idx);
        uint64_t any    = bucket_find_locked(h, bk, key, klen, 0, NULL);
        if (any != OFFSET_NULL) {
            NameEntry *ne = (NameEntry *)OFF2PTR(h, any);
            if (ne->type != ENTRY_HASH) {
                pthread_mutex_unlock(&bk->mutex);
                LOG_ERR("HSET: '%.*s' 다른 타입 존재 type=%u", klen, (const char *)key, ne->type);
                return reply_error(SHM_ERR_KEY_EXISTS, shm_strerror(SHM_ERR_KEY_EXISTS));
            }
        }
        pthread_mutex_unlock(&bk->mutex);
    }

    int err = SHM_OK;
    HashHeader *hh = hash_get_or_create(h, key, klen, &err);
    if (!hh) return reply_error(err, shm_strerror(err));

    int rc = pthread_mutex_lock(&hh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&hh->mutex);

    int64_t added = 0;
    for (uint32_t i = 2; i + 1 < argc; i += 2) {
        const void *field = args[i]->ptr;   uint32_t flen = args[i]->len;
        const void *val   = args[i+1]->ptr; uint32_t vlen = args[i+1]->len;
        if (flen == 0) continue;

        uint64_t prev   = OFFSET_NULL;
        uint64_t fe_off = field_find_locked(h, hh, field, flen, &prev);

        if (fe_off != OFFSET_NULL) {
            /* 기존 field 값 갱신 */
            FieldEntry *fe = (FieldEntry *)OFF2PTR(h, fe_off);
            heap_free(h, fe->val_offset);
            uint64_t nvo = heap_alloc(h, vlen > 0 ? vlen : 1);
            if (nvo == OFFSET_NULL) {
                pthread_mutex_unlock(&hh->mutex);
                return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
            }
            if (vlen > 0) memcpy(OFF2PTR(h, nvo), val, vlen);
            fe->val_offset = nvo; fe->val_len = vlen;
            LOG_TRACE("HSET: 갱신 field='%.*s'", flen, (const char *)field);
        } else {
            /* 신규 field 추가 */
            uint64_t foff = get_or_intern_field(h, field, flen);
            uint64_t voff = heap_alloc(h, vlen > 0 ? vlen : 1);
            uint64_t eoff = heap_alloc(h, sizeof(FieldEntry));
            if (foff == OFFSET_NULL || voff == OFFSET_NULL || eoff == OFFSET_NULL) {
                if (foff != OFFSET_NULL) decrease_field_ref(h, OFF2PTR(h, foff), flen);
                if (voff != OFFSET_NULL) heap_free(h, voff);
                if (eoff != OFFSET_NULL) heap_free(h, eoff);
                pthread_mutex_unlock(&hh->mutex);
                return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
            }
            if (vlen > 0) memcpy(OFF2PTR(h, voff), val, vlen);

            FieldEntry *fe = (FieldEntry *)OFF2PTR(h, eoff);
            fe->field_offset = foff; fe->field_len = flen;
            fe->val_offset   = voff; fe->val_len   = vlen;

            uint64_t *bkts = hh_field_buckets(hh);
            uint32_t bi    = shm_field_hash(field, flen, hh->n_buckets);
            fe->next_offset = bkts[bi];
            bkts[bi]        = eoff;
            hh->field_count++;
            added++;
            LOG_TRACE("HSET: 추가 field='%.*s' fc=%lu", flen, (const char *)field, hh->field_count);
        }
    }
    pthread_mutex_unlock(&hh->mutex);
    return reply_integer(added);
}

/* ============================================================
 *  HGET  key field
 *  반환: STRING value | NIL
 * ============================================================ */
s_replyObject *cmd_hget(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3) return reply_error(SHM_ERR_ARGC, "usage: HGET key field");
    const void *key   = args[1]->ptr; uint32_t klen = args[1]->len;
    const void *field = args[2]->ptr; uint32_t flen = args[2]->len;

    HashHeader *hh = core_hash_get(h, key, klen);
    if (!hh) return reply_nil();

    int rc = pthread_mutex_lock(&hh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&hh->mutex);

    uint64_t fe_off = field_find_locked(h, hh, field, flen, NULL);
    if (fe_off == OFFSET_NULL) {
        pthread_mutex_unlock(&hh->mutex);
        return reply_nil();
    }
    FieldEntry *fe = (FieldEntry *)OFF2PTR(h, fe_off);
    const char *vp = fe->val_len > 0
                     ? (const char *)OFF2PTR(h, fe->val_offset) : "";
    s_replyObject *r = reply_string(vp, fe->val_len);
    pthread_mutex_unlock(&hh->mutex);
    return r;
}

/* ============================================================
 *  HDEL  key field [field …]
 *  반환: INTEGER 삭제 수
 * ============================================================ */
s_replyObject *cmd_hdel(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3) return reply_error(SHM_ERR_ARGC, "usage: HDEL key field [field …]");
    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;

    HashHeader *hh = core_hash_get(h, key, klen);
    if (!hh) return reply_integer(0);

    int rc = pthread_mutex_lock(&hh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&hh->mutex);

    uint64_t *bkts  = hh_field_buckets(hh);
    int64_t   deleted = 0;

    for (uint32_t a = 2; a < argc; a++) {
        const void *field = args[a]->ptr; uint32_t flen = args[a]->len;
        if (flen == 0) continue;
        uint32_t bi   = shm_field_hash(field, flen, hh->n_buckets);
        uint64_t prev = OFFSET_NULL, cur = bkts[bi];
        while (cur != OFFSET_NULL) {
            FieldEntry *fe = (FieldEntry *)OFF2PTR(h, cur);
            if (fe->field_len == flen &&
                memcmp(OFF2PTR(h, fe->field_offset), field, flen) == 0) {
                if (prev == OFFSET_NULL) bkts[bi] = fe->next_offset;
                else ((FieldEntry *)OFF2PTR(h, prev))->next_offset = fe->next_offset;
				decrease_field_ref(h, OFF2PTR(h, fe->field_offset), fe->field_len);
                heap_free(h, fe->val_offset);
                heap_free(h, cur);
                hh->field_count--;
                deleted++;
                break;
            }
            prev = cur; cur = fe->next_offset;
        }
    }
    pthread_mutex_unlock(&hh->mutex);
    return reply_integer(deleted);
}

/* ============================================================
 *  HEXISTS  key field
 *  반환: INTEGER 1|0
 * ============================================================ */
s_replyObject *cmd_hexists(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3) return reply_error(SHM_ERR_ARGC, "usage: HEXISTS key field");
    HashHeader *hh = core_hash_get(h, args[1]->ptr, args[1]->len);
    if (!hh) return reply_integer(0);

    int rc = pthread_mutex_lock(&hh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&hh->mutex);
    int exists = (field_find_locked(h, hh, args[2]->ptr, args[2]->len, NULL)
                  != OFFSET_NULL) ? 1 : 0;
    pthread_mutex_unlock(&hh->mutex);
    return reply_integer(exists);
}

/* ============================================================
 *  HLEN  key
 *  반환: INTEGER field 수
 * ============================================================ */
s_replyObject *cmd_hlen(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: HLEN key");
    HashHeader *hh = core_hash_get(h, args[1]->ptr, args[1]->len);
    return reply_integer(hh ? (int64_t)hh->field_count : 0);
}

/* ============================================================
 *  내부: 전체 field 순회 → ARRAY 생성
 *    mode 0 = field + value (HGETALL)
 *    mode 1 = field only    (HKEYS)
 *    mode 2 = value only    (HVALS)
 * ============================================================ */
static s_replyObject *hash_scan(ShmHandle *h, HashHeader *hh, int mode)
{
    s_replyObject *arr = reply_array((size_t)(hh->field_count * 2));
    if (!arr) return NULL;
    uint64_t *bkts = hh_field_buckets(hh);
    for (uint32_t i = 0; i < hh->n_buckets; i++) {
        uint64_t cur = bkts[i];
        while (cur != OFFSET_NULL) {
            FieldEntry *fe = (FieldEntry *)OFF2PTR(h, cur);
            if (mode == 0 || mode == 1) {
                /* field 이름 */
                s_replyObject *fobj = reply_string(
                    (const char *)OFF2PTR(h, fe->field_offset), fe->field_len);
                if (!fobj) { reply_free(arr); return NULL; }
                reply_array_append(arr, fobj);
            }
            if (mode == 0 || mode == 2) {
                /* value */
                const char *vp = fe->val_len > 0
                                 ? (const char *)OFF2PTR(h, fe->val_offset) : "";
                s_replyObject *vobj = reply_string(vp, fe->val_len);
                if (!vobj) { reply_free(arr); return NULL; }
                reply_array_append(arr, vobj);
            }
            cur = fe->next_offset;
        }
    }
    return arr;
}

/* ============================================================
 *  HGETALL  key
 *  반환: ARRAY [field1 val1 field2 val2 …]
 * ============================================================ */
s_replyObject *cmd_hgetall(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: HGETALL key");
    HashHeader *hh = core_hash_get(h, args[1]->ptr, args[1]->len);
    if (!hh) return reply_array(0);
    int rc = pthread_mutex_lock(&hh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&hh->mutex);
    s_replyObject *r = hash_scan(h, hh, 0);
    pthread_mutex_unlock(&hh->mutex);
    return r ? r : reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
}

/* ============================================================
 *  HKEYS  key
 *  반환: ARRAY [field1 field2 …]
 * ============================================================ */
s_replyObject *cmd_hkeys(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: HKEYS key");
    HashHeader *hh = core_hash_get(h, args[1]->ptr, args[1]->len);
    if (!hh) return reply_array(0);
    int rc = pthread_mutex_lock(&hh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&hh->mutex);
    s_replyObject *r = hash_scan(h, hh, 1);
    pthread_mutex_unlock(&hh->mutex);
    return r ? r : reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
}

/* ============================================================
 *  HVALS  key
 *  반환: ARRAY [val1 val2 …]
 * ============================================================ */
s_replyObject *cmd_hvals(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: HVALS key");
    HashHeader *hh = core_hash_get(h, args[1]->ptr, args[1]->len);
    if (!hh) return reply_array(0);
    int rc = pthread_mutex_lock(&hh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&hh->mutex);
    s_replyObject *r = hash_scan(h, hh, 2);
    pthread_mutex_unlock(&hh->mutex);
    return r ? r : reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
}

/* ============================================================
 *  내부: field 의 숫자 값 읽기 / 쓰기
 * ============================================================ */
static int64_t hfield_read_int(ShmHandle *h, HashHeader *hh,
                                const void *field, uint32_t flen)
{
    uint64_t fe_off = field_find_locked(h, hh, field, flen, NULL);
    if (fe_off == OFFSET_NULL) return 0;
    FieldEntry *fe = (FieldEntry *)OFF2PTR(h, fe_off);
    if (fe->val_len == 0) return 0;
    char buf[32];
    uint32_t cp = fe->val_len < 31 ? fe->val_len : 31;
    memcpy(buf, OFF2PTR(h, fe->val_offset), cp); buf[cp] = '\0';
    return (int64_t)strtoll(buf, NULL, 10);
}

static double hfield_read_float(ShmHandle *h, HashHeader *hh,
                                 const void *field, uint32_t flen)
{
    uint64_t fe_off = field_find_locked(h, hh, field, flen, NULL);
    if (fe_off == OFFSET_NULL) return 0.0;
    FieldEntry *fe = (FieldEntry *)OFF2PTR(h, fe_off);
    if (fe->val_len == 0) return 0.0;
    char buf[64];
    uint32_t cp = fe->val_len < 63 ? fe->val_len : 63;
    memcpy(buf, OFF2PTR(h, fe->val_offset), cp); buf[cp] = '\0';
    return strtod(buf, NULL);
}

static int hfield_write_str(ShmHandle *h, HashHeader *hh,
                              const void *field, uint32_t flen,
                              const char *sval, uint32_t slen)
{
    uint64_t *bkts  = hh_field_buckets(hh);
    uint64_t fe_off = field_find_locked(h, hh, field, flen, NULL);

    if (fe_off != OFFSET_NULL) {
        FieldEntry *fe = (FieldEntry *)OFF2PTR(h, fe_off);
        heap_free(h, fe->val_offset);
        uint64_t nvo = heap_alloc(h, slen + 1);
        if (nvo == OFFSET_NULL) return SHM_ERR_NOMEM;
        memcpy(OFF2PTR(h, nvo), sval, slen);
        ((char *)OFF2PTR(h, nvo))[slen] = '\0';
        fe->val_offset = nvo; fe->val_len = slen;
        return SHM_OK;
    }
    /* 신규 */
	uint64_t foff = get_or_intern_field(h, field, flen);
    uint64_t voff = heap_alloc(h, slen + 1);
    uint64_t eoff = heap_alloc(h, sizeof(FieldEntry));
    if (foff == OFFSET_NULL || voff == OFFSET_NULL || eoff == OFFSET_NULL) {
        if (foff != OFFSET_NULL) decrease_field_ref(h, OFF2PTR(h, foff), flen);
        if (voff != OFFSET_NULL) heap_free(h, voff);
        if (eoff != OFFSET_NULL) heap_free(h, eoff);
        return SHM_ERR_NOMEM;
    }
    memcpy(OFF2PTR(h, voff), sval, slen);
    ((char *)OFF2PTR(h, voff))[slen] = '\0';

    FieldEntry *fe = (FieldEntry *)OFF2PTR(h, eoff);
    fe->field_offset = foff; fe->field_len = flen;
    fe->val_offset   = voff; fe->val_len   = slen;

    uint32_t bi     = shm_field_hash(field, flen, hh->n_buckets);
    fe->next_offset = bkts[bi];
    bkts[bi]        = eoff;
    hh->field_count++;
    return SHM_OK;
}

/* ============================================================
 *  HINCRBY  key field delta
 *  반환: INTEGER 새 값
 * ============================================================ */
s_replyObject *cmd_hincrby(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 4) return reply_error(SHM_ERR_ARGC, "usage: HINCRBY key field delta");
    const void *key   = args[1]->ptr; uint32_t klen = args[1]->len;
    const void *field = args[2]->ptr; uint32_t flen = args[2]->len;
    char *ep = NULL;
    int64_t delta = strtoll(args[3]->ptr, &ep, 10);
    if (ep == args[3]->ptr) return reply_error(SHM_ERR_PARSE, "delta 변환 실패");

    int err = SHM_OK;
    HashHeader *hh = hash_get_or_create(h, key, klen, &err);
    if (!hh) return reply_error(err, shm_strerror(err));

    int rc = pthread_mutex_lock(&hh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&hh->mutex);

    int64_t cur_val  = hfield_read_int(h, hh, field, flen);
    int64_t new_val  = cur_val + delta;
    char    sbuf[32]; int slen = snprintf(sbuf, sizeof(sbuf), "%lld", (long long)new_val);
    int     r = hfield_write_str(h, hh, field, flen, sbuf, (uint32_t)slen);
    pthread_mutex_unlock(&hh->mutex);
    if (r != SHM_OK) return reply_error(r, shm_strerror(r));
    return reply_integer(new_val);
}

/* ============================================================
 *  HINCRBYFLOAT  key field delta
 *  반환: STRING 새 값 (Redis 호환)
 * ============================================================ */
s_replyObject *cmd_hincrbyfloat(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 4) return reply_error(SHM_ERR_ARGC, "usage: HINCRBYFLOAT key field delta");
    const void *key   = args[1]->ptr; uint32_t klen = args[1]->len;
    const void *field = args[2]->ptr; uint32_t flen = args[2]->len;
    char *ep = NULL;
    double delta = strtod(args[3]->ptr, &ep);
    if (ep == args[3]->ptr) return reply_error(SHM_ERR_PARSE, "delta 변환 실패");

    int err = SHM_OK;
    HashHeader *hh = hash_get_or_create(h, key, klen, &err);
    if (!hh) return reply_error(err, shm_strerror(err));

    int rc = pthread_mutex_lock(&hh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&hh->mutex);

    double cur_val = hfield_read_float(h, hh, field, flen);
    double new_val = cur_val + delta;
    char   sbuf[64]; int slen = snprintf(sbuf, sizeof(sbuf), "%.17g", new_val);
    int    r = hfield_write_str(h, hh, field, flen, sbuf, (uint32_t)slen);
    pthread_mutex_unlock(&hh->mutex);
    if (r != SHM_OK) return reply_error(r, shm_strerror(r));
    return reply_string(sbuf, (size_t)slen);
}
