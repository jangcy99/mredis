/*
 * cmd_set.c  –  Redis Set 완전 구현 (SREM, SISMEMBER, SPOP, SRANDMEMBER 포함)
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "shm_types.h"
#include "shm_core.h"
#include "cmd_set.h"

#define SET_BUCKETS 32

static inline uint64_t set_alloc_size(void)
{
    return sizeof(SetHeader) + (uint64_t)SET_BUCKETS * sizeof(uint64_t);
}

/* SetHeader 신규 생성 */
static uint64_t setheader_new(ShmHandle *h)
{
    uint64_t off = heap_alloc(h, set_alloc_size());
    if (off == OFFSET_NULL) return OFFSET_NULL;

    SetHeader *sh = (SetHeader *)OFF2PTR(h, off);
    sh->member_count = 0;
    sh->n_buckets = SET_BUCKETS;
    sh->pad = 0;

    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&sh->mutex, &ma);
    pthread_mutexattr_destroy(&ma);

    uint64_t *buckets = (uint64_t*)(sh + 1);
    for (uint32_t i = 0; i < SET_BUCKETS; i++)
        buckets[i] = OFFSET_NULL;

    return off;
}

/* Set 가져오기 */
static SetHeader *set_get(ShmHandle *h, const void *key, uint32_t klen)
{
    uint64_t ne = bucket_find(h, shm_hash(key, klen), key, klen, ENTRY_SET, NULL);
    if (ne == OFFSET_NULL) return NULL;
    return (SetHeader *)OFF2PTR(h, ((NameEntry*)OFF2PTR(h, ne))->data_offset);
}

/* Set 가져오기 또는 생성 */
static SetHeader *set_get_or_create(ShmHandle *h, const void *key, uint32_t klen, int *err_out)
{
    SetHeader *sh = set_get(h, key, klen);
    if (sh) { *err_out = SHM_OK; return sh; }

    uint32_t idx = shm_hash(key, klen);
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t ne_off = OFFSET_NULL;
    int chk = type_check(h, bk, key, klen, ENTRY_SET, &ne_off);

    if (chk == SHM_ERR_KEY_EXISTS) {
        pthread_mutex_unlock(&bk->mutex);
        *err_out = SHM_ERR_KEY_EXISTS;
        return NULL;
    }
    if (chk == 0) {
        sh = (SetHeader *)OFF2PTR(h, ((NameEntry*)OFF2PTR(h, ne_off))->data_offset);
        pthread_mutex_unlock(&bk->mutex);
        *err_out = SHM_OK;
        return sh;
    }

    uint64_t sh_off = setheader_new(h);
    if (sh_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        *err_out = SHM_ERR_NOMEM;
        return NULL;
    }

    uint64_t new_ne = nameentry_alloc(h, key, klen, ENTRY_SET, sh_off);
    if (new_ne == OFFSET_NULL) {
        heap_free(h, sh_off);
        pthread_mutex_unlock(&bk->mutex);
        *err_out = SHM_ERR_NOMEM;
        return NULL;
    }

    ((NameEntry *)OFF2PTR(h, new_ne))->next_offset = bk->head_offset;
    bk->head_offset = new_ne;
    pthread_mutex_unlock(&bk->mutex);

    *err_out = SHM_OK;
    return (SetHeader *)OFF2PTR(h, sh_off);
}

/* ============================================================
 *  SCREATE / SDROP
 * ============================================================ */
s_replyObject *cmd_screate(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: SCREATE key");
    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;

    uint32_t idx = shm_hash(key, klen);
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t ne_off = OFFSET_NULL;
    int chk = type_check(h, bk, key, klen, ENTRY_SET, &ne_off);

    if (chk == SHM_ERR_KEY_EXISTS) {
        pthread_mutex_unlock(&bk->mutex);
        return reply_error(SHM_ERR_KEY_EXISTS, shm_strerror(SHM_ERR_KEY_EXISTS));
    }
    if (chk == 0) {
        pthread_mutex_unlock(&bk->mutex);
        return reply_ok();
    }

    uint64_t sh_off = setheader_new(h);
    if (sh_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
    }

    uint64_t new_ne = nameentry_alloc(h, key, klen, ENTRY_SET, sh_off);
    if (new_ne == OFFSET_NULL) {
        heap_free(h, sh_off);
        pthread_mutex_unlock(&bk->mutex);
        return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
    }

    ((NameEntry *)OFF2PTR(h, new_ne))->next_offset = bk->head_offset;
    bk->head_offset = new_ne;
    pthread_mutex_unlock(&bk->mutex);
    return reply_ok();
}

s_replyObject *cmd_sdrop(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: SDROP key");
    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;

    uint32_t idx = shm_hash(key, klen);
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t prev = OFFSET_NULL;
    uint64_t ne_off = bucket_find_locked(h, bk, key, klen, ENTRY_SET, &prev);
    if (ne_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return reply_error(SHM_ERR_NOT_FOUND, shm_strerror(SHM_ERR_NOT_FOUND));
    }

    NameEntry *ne = (NameEntry *)OFF2PTR(h, ne_off);
    uint64_t sh_off = ne->data_offset;
    SetHeader *sh = (SetHeader *)OFF2PTR(h, sh_off);

    if (prev == OFFSET_NULL)
        bk->head_offset = ne->next_offset;
    else
        ((NameEntry *)OFF2PTR(h, prev))->next_offset = ne->next_offset;

    nameentry_free(h, ne_off);
    pthread_mutex_unlock(&bk->mutex);

    /* 내부 멤버 정리 */
    pthread_mutex_lock(&sh->mutex);
    uint64_t *buckets = (uint64_t*)(sh + 1);
    for (uint32_t i = 0; i < sh->n_buckets; i++) {
        uint64_t cur = buckets[i];
        while (cur != OFFSET_NULL) {
            SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
            uint64_t next = se->next_offset;
            heap_free(h, se->member_offset);
            heap_free(h, cur);
            cur = next;
        }
    }
    pthread_mutex_unlock(&sh->mutex);
    pthread_mutex_destroy(&sh->mutex);
    heap_free(h, sh_off);

    return reply_ok();
}

/* ============================================================
 *  SADD
 * ============================================================ */
s_replyObject *cmd_sadd(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3)
        return reply_error(SHM_ERR_ARGC, "usage: SADD key member [member ...]");

    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;
    int err;
    SetHeader *sh = set_get_or_create(h, key, klen, &err);
    if (!sh) return reply_error(err, shm_strerror(err));

    int rc = pthread_mutex_lock(&sh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&sh->mutex);

    int added = 0;
    uint64_t *buckets = (uint64_t*)(sh + 1);

    for (uint32_t i = 2; i < argc; i++) {
        const void *member = args[i]->ptr;
        uint32_t mlen = args[i]->len;
        if (mlen == 0) continue;

        uint32_t bi = shm_field_hash(member, mlen, sh->n_buckets);
        uint64_t cur = buckets[bi];
        int exists = 0;

        while (cur != OFFSET_NULL) {
            SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
            if (se->member_len == mlen && memcmp(OFF2PTR(h, se->member_offset), member, mlen) == 0) {
                exists = 1;
                break;
            }
            cur = se->next_offset;
        }

        if (!exists) {
            uint64_t moff = heap_alloc(h, mlen);
            uint64_t soff = heap_alloc(h, sizeof(SetEntry));
            if (moff != OFFSET_NULL && soff != OFFSET_NULL) {
                memcpy(OFF2PTR(h, moff), member, mlen);
                SetEntry *se = (SetEntry *)OFF2PTR(h, soff);
                se->member_offset = moff;
                se->member_len = mlen;
                se->next_offset = buckets[bi];
                buckets[bi] = soff;
                sh->member_count++;
                added++;
            } else {
                if (moff) heap_free(h, moff);
                if (soff) heap_free(h, soff);
            }
        }
    }

    pthread_mutex_unlock(&sh->mutex);
    return reply_integer(added);
}

/* ============================================================
 *  SISMEMBER
 * ============================================================ */
s_replyObject *cmd_sismember(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3)
        return reply_error(SHM_ERR_ARGC, "usage: SISMEMBER key member");

    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;
    const void *member = args[2]->ptr; uint32_t mlen = args[2]->len;

    SetHeader *sh = set_get(h, key, klen);
    if (!sh) return reply_integer(0);

    int rc = pthread_mutex_lock(&sh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&sh->mutex);

    uint64_t *buckets = (uint64_t*)(sh + 1);
    uint32_t bi = shm_field_hash(member, mlen, sh->n_buckets);
    uint64_t cur = buckets[bi];
    int found = 0;

    while (cur != OFFSET_NULL) {
        SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
        if (se->member_len == mlen && memcmp(OFF2PTR(h, se->member_offset), member, mlen) == 0) {
            found = 1;
            break;
        }
        cur = se->next_offset;
    }

    pthread_mutex_unlock(&sh->mutex);
    return reply_integer(found ? 1 : 0);
}

/* ============================================================
 *  SCARD, SMEMBERS
 * ============================================================ */
s_replyObject *cmd_scard(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: SCARD key");
    SetHeader *sh = set_get(h, args[1]->ptr, args[1]->len);
    return reply_integer(sh ? (int64_t)sh->member_count : 0);
}

s_replyObject *cmd_smembers(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: SMEMBERS key");
    SetHeader *sh = set_get(h, args[1]->ptr, args[1]->len);
    if (!sh) return reply_array(0);

    s_replyObject *arr = reply_array(sh->member_count);
    if (!arr) return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));

    uint64_t *buckets = (uint64_t*)(sh + 1);
    for (uint32_t i = 0; i < sh->n_buckets; i++) {
        uint64_t cur = buckets[i];
        while (cur != OFFSET_NULL) {
            SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
            s_replyObject *m = reply_string((const char *)OFF2PTR(h, se->member_offset), se->member_len);
            reply_array_append(arr, m);
            cur = se->next_offset;
        }
    }
    return arr;
}

/* ============================================================
 *  SREM - robust 버전
 * ============================================================ */
s_replyObject *cmd_srem(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3)
        return reply_error(SHM_ERR_ARGC, "usage: SREM key member [member ...]");

    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;
    SetHeader *sh = set_get(h, key, klen);
    if (!sh) return reply_integer(0);

    int rc = pthread_mutex_lock(&sh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&sh->mutex);

    int removed = 0;
    uint64_t *buckets = (uint64_t*)(sh + 1);

    for (uint32_t i = 2; i < argc; i++) {
        const void *member = args[i]->ptr;
        uint32_t mlen = args[i]->len;
        if (mlen == 0) continue;

        uint32_t bi = shm_field_hash(member, mlen, sh->n_buckets);
        uint64_t prev = OFFSET_NULL;
        uint64_t cur = buckets[bi];

        while (cur != OFFSET_NULL) {
            SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
            if (se->member_len == mlen &&
                memcmp(OFF2PTR(h, se->member_offset), member, mlen) == 0) {

                if (prev == OFFSET_NULL)
                    buckets[bi] = se->next_offset;
                else
                    ((SetEntry *)OFF2PTR(h, prev))->next_offset = se->next_offset;

                heap_free(h, se->member_offset);
                heap_free(h, cur);

                sh->member_count--;
                removed++;
                break;
            }
            prev = cur;
            cur = se->next_offset;
        }
    }

    pthread_mutex_unlock(&sh->mutex);
    return reply_integer(removed);
}

/* ============================================================
 *  SPOP - robust 버전
 * ============================================================ */
s_replyObject *cmd_spop(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2)
        return reply_error(SHM_ERR_ARGC, "usage: SPOP key [count]");

    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;
    int count = (argc >= 3) ? atoi(args[2]->ptr) : 1;
    if (count < 1) count = 1;

    SetHeader *sh = set_get(h, key, klen);
    if (!sh || sh->member_count == 0) {
        return (count == 1) ? reply_nil() : reply_array(0);
    }

    int rc = pthread_mutex_lock(&sh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&sh->mutex);

    if (count > (int)sh->member_count)
        count = (int)sh->member_count;

    s_replyObject *arr = reply_array(count);
    if (!arr) {
        pthread_mutex_unlock(&sh->mutex);
        return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
    }

    uint64_t *buckets = (uint64_t*)(sh + 1);
    int popped = 0;

    while (popped < count && sh->member_count > 0) {
        uint32_t bi = rand() % sh->n_buckets;
        uint64_t prev = OFFSET_NULL;
        uint64_t cur = buckets[bi];

        // 빈 버킷이면 다른 버킷 찾기
        if (cur == OFFSET_NULL) {
            for (uint32_t j = 1; j < sh->n_buckets; j++) {
                uint32_t nbi = (bi + j) % sh->n_buckets;
                if (buckets[nbi] != OFFSET_NULL) {
                    bi = nbi;
                    cur = buckets[bi];
                    break;
                }
            }
        }

        if (cur != OFFSET_NULL) {
            SetEntry *se = (SetEntry *)OFF2PTR(h, cur);

            // 반환
            s_replyObject *m = reply_string((const char *)OFF2PTR(h, se->member_offset), se->member_len);
            reply_array_append(arr, m);

            // 제거
            if (prev == OFFSET_NULL)
                buckets[bi] = se->next_offset;
            else
                ((SetEntry *)OFF2PTR(h, prev))->next_offset = se->next_offset;

            heap_free(h, se->member_offset);
            heap_free(h, cur);

            sh->member_count--;
            popped++;
        }
    }

    pthread_mutex_unlock(&sh->mutex);
    return arr;
}

s_replyObject *cmd_srandmember(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2)
        return reply_error(SHM_ERR_ARGC, "usage: SRANDMEMBER key [count]");

    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;
    int count = (argc >= 3) ? atoi(args[2]->ptr) : 1;
    if (count < 1) count = 1;

    SetHeader *sh = set_get(h, key, klen);
    if (!sh || sh->member_count == 0) {
        return (count == 1) ? reply_nil() : reply_array(0);
    }

    int rc = pthread_mutex_lock(&sh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&sh->mutex);

    if (count == 1) {
        // 단일 반환
        uint32_t bi = rand() % sh->n_buckets;
        uint64_t cur = ((uint64_t*)(sh + 1))[bi];
        if (cur == OFFSET_NULL) {
            for (uint32_t j = 1; j < sh->n_buckets; j++) {
                if (((uint64_t*)(sh + 1))[(bi + j) % sh->n_buckets] != OFFSET_NULL) {
                    cur = ((uint64_t*)(sh + 1))[(bi + j) % sh->n_buckets];
                    break;
                }
            }
        }
        if (cur != OFFSET_NULL) {
            SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
            s_replyObject *ret = reply_string((const char *)OFF2PTR(h, se->member_offset), se->member_len);
            pthread_mutex_unlock(&sh->mutex);
            return ret;
        }
        pthread_mutex_unlock(&sh->mutex);
        return reply_nil();
    }
    else {
        // 배열 반환
        s_replyObject *arr = reply_array(count);
        if (!arr) {
            pthread_mutex_unlock(&sh->mutex);
            return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
        }

        uint64_t *buckets = (uint64_t*)(sh + 1);
        for (int i = 0; i < count; i++) {
            uint32_t bi = rand() % sh->n_buckets;
            uint64_t cur = buckets[bi];
            if (cur != OFFSET_NULL) {
                SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
                reply_array_append(arr, reply_string((const char *)OFF2PTR(h, se->member_offset), se->member_len));
            } else {
                reply_array_append(arr, reply_nil());
            }
        }
        pthread_mutex_unlock(&sh->mutex);
        return arr;
    }
}
