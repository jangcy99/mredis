/*
 * cmd_set.c  –  Redis Set 완전 구현 (SREM, SISMEMBER, SPOP, SRANDMEMBER 포함)
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "mredis_types.h"
#include "mredis_core.h"
#include "cmd_set.h"

/*
 * cmd_set.c – 의존성 및 오프셋 경계 완전 고정화 버전
 */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include "mredis_types.h"
#include "mredis_core.h"
#include "cmd_set.h"

static inline uint64_t set_alloc_size(void)
{
    return sizeof(SetHeader);
}

/* SetHeader 신규 생성 (바이트 평면 완전 청소 버전) */
static uint64_t setheader_new(MRedisHandle *h)
{
    uint64_t off = heap_alloc(h, set_alloc_size());
    if (off == OFFSET_NULL) return OFFSET_NULL;

    SetHeader *sh = (SetHeader *)OFF2PTR(h, off);
    // 구조체 전체 영역을 0으로 밀어 쓰레기 값 개입을 원천 차단
    memset(sh, 0, sizeof(SetHeader));

    sh->member_count = 0;
    sh->n_buckets = SET_BUCKETS;
    sh->data_offset = OFFSET_NULL;

    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&sh->mutex, &ma);
    pthread_mutexattr_destroy(&ma);

    // 명시적 오프셋 NULL 대입
    for (uint32_t i = 0; i < SET_BUCKETS; i++) {
        sh->buckets[i] = OFFSET_NULL;
    }

    return off;
}

static SetHeader *set_get_locked(MRedisHandle *h, const void *key, uint32_t klen, BucketEntry *bk)
{
    uint64_t prev = OFFSET_NULL;
    uint64_t ne_off = bucket_find_locked(h, bk, key, klen, ENTRY_SET, &prev);
    if (ne_off == OFFSET_NULL) return NULL;

    NameEntry *ne = (NameEntry *)OFF2PTR(h, ne_off);
    return (SetHeader *)OFF2PTR(h, ne->data_offset);
}

static SetHeader *set_get_or_create(MRedisHandle *h, const void *key, uint32_t klen, int *err_out)
{
    uint32_t idx = mredis_hash(key, klen) % ((MRedisHeader*)(h->base))->hash_table_size;
    BucketEntry *bk = core_get_bucket(h, idx);

    bucket_lock(h, idx);

    uint64_t ne_off = bucket_find_locked(h, bk, key, klen, 0, NULL);
    if (ne_off != OFFSET_NULL) {
        NameEntry *ne = (NameEntry *)OFF2PTR(h, ne_off);
        if (ne->type != ENTRY_SET) {
            pthread_mutex_unlock(&bk->mutex);
            *err_out = SHM_ERR_KEY_EXISTS;
            return NULL;
        }
        SetHeader *sh = (SetHeader *)OFF2PTR(h, ne->data_offset);
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

    SetHeader *sh = (SetHeader *)OFF2PTR(h, sh_off);
    pthread_mutex_unlock(&bk->mutex);

    *err_out = SHM_OK;
    return sh;
}

/* ============================================================
 * SADD
 * ============================================================ */
s_replyObject *cmd_sadd(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3) return reply_error(SHM_ERR_ARGC, "ERR syntax");

    int err;
    SetHeader *sh = set_get_or_create(h, args[1]->ptr, args[1]->len, &err);
    if (!sh) return reply_error(err, "ERR allocation failed");

    int rc = pthread_mutex_lock(&sh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&sh->mutex);

    int added = 0;
    for (uint32_t i = 2; i < argc; i++) {
        const void *member = args[i]->ptr;
        uint32_t mlen = args[i]->len;
        if (mlen == 0) continue;

        uint32_t bi = mredis_field_hash(member, mlen, sh->n_buckets);
        uint64_t cur = sh->buckets[bi];
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
                se->next_offset = sh->buckets[bi];
                sh->buckets[bi] = soff;
                sh->member_count++;
                added++;
            } else {
                if (moff != OFFSET_NULL) heap_free(h, moff);
                if (soff != OFFSET_NULL) heap_free(h, soff);
            }
        }
    }
    pthread_mutex_unlock(&sh->mutex);
    return reply_integer(added);
}

/* ============================================================
 * SREM
 * ============================================================ */
s_replyObject *cmd_srem(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3) return reply_error(SHM_ERR_ARGC, "ERR syntax");

    uint32_t idx = mredis_hash(args[1]->ptr, args[1]->len) % ((MRedisHeader*)(h->base))->hash_table_size;
    BucketEntry *bk = core_get_bucket(h, idx);

    bucket_lock(h, idx);
    SetHeader *sh = set_get_locked(h, args[1]->ptr, args[1]->len, bk);
    if (!sh) {
        pthread_mutex_unlock(&bk->mutex);
        return reply_integer(0);
    }

    int rc = pthread_mutex_lock(&sh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&sh->mutex);
    pthread_mutex_unlock(&bk->mutex);

    int removed = 0;
    for (uint32_t i = 2; i < argc; i++) {
        const void *member = args[i]->ptr;
        uint32_t mlen = args[i]->len;

        uint32_t bi = mredis_field_hash(member, mlen, sh->n_buckets);
        uint64_t cur = sh->buckets[bi];
        uint64_t prev = OFFSET_NULL;

        while (cur != OFFSET_NULL) {
            SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
            if (se->member_len == mlen && memcmp(OFF2PTR(h, se->member_offset), member, mlen) == 0) {
                uint64_t next = se->next_offset;
                if (prev == OFFSET_NULL) {
                    sh->buckets[bi] = next;
                } else {
                    ((SetEntry *)OFF2PTR(h, prev))->next_offset = next;
                }
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
 * SISMEMBER / SCARD / SMEMBERS
 * ============================================================ */
s_replyObject *cmd_sismember(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3) return reply_error(SHM_ERR_ARGC, "ERR syntax");
    uint32_t idx = mredis_hash(args[1]->ptr, args[1]->len) % ((MRedisHeader*)(h->base))->hash_table_size;
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);
    SetHeader *sh = set_get_locked(h, args[1]->ptr, args[1]->len, bk);
    if (!sh) { pthread_mutex_unlock(&bk->mutex); return reply_integer(0); }
    int rc = pthread_mutex_lock(&sh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&sh->mutex);
    pthread_mutex_unlock(&bk->mutex);

    uint32_t bi = mredis_field_hash(args[2]->ptr, args[2]->len, sh->n_buckets);
    uint64_t cur = sh->buckets[bi];
    int found = 0;
    while (cur != OFFSET_NULL) {
        SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
        if (se->member_len == args[2]->len && memcmp(OFF2PTR(h, se->member_offset), args[2]->ptr, se->member_len) == 0) {
            found = 1; break;
        }
        cur = se->next_offset;
    }
    pthread_mutex_unlock(&sh->mutex);
    return reply_integer(found);
}

s_replyObject *cmd_scard(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "ERR syntax");
    uint32_t idx = mredis_hash(args[1]->ptr, args[1]->len) % ((MRedisHeader*)(h->base))->hash_table_size;
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);
    SetHeader *sh = set_get_locked(h, args[1]->ptr, args[1]->len, bk);
    if (!sh) { pthread_mutex_unlock(&bk->mutex); return reply_integer(0); }
    int rc = pthread_mutex_lock(&sh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&sh->mutex);
    uint32_t count = sh->member_count;
    pthread_mutex_unlock(&sh->mutex);
    pthread_mutex_unlock(&bk->mutex);
    return reply_integer(count);
}

s_replyObject *cmd_smembers(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "ERR syntax");
    uint32_t idx = mredis_hash(args[1]->ptr, args[1]->len) % ((MRedisHeader*)(h->base))->hash_table_size;
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);
    SetHeader *sh = set_get_locked(h, args[1]->ptr, args[1]->len, bk);
    if (!sh) { pthread_mutex_unlock(&bk->mutex); return reply_array(0); }
    int rc = pthread_mutex_lock(&sh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&sh->mutex);
    pthread_mutex_unlock(&bk->mutex);

    s_replyObject *arr = reply_array(sh->member_count);
    for (uint32_t b = 0; b < sh->n_buckets; b++) {
        uint64_t cur = sh->buckets[b];
        while (cur != OFFSET_NULL) {
            SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
            reply_array_append(arr, reply_string(OFF2PTR(h, se->member_offset), se->member_len));
            cur = se->next_offset;
        }
    }
    pthread_mutex_unlock(&sh->mutex);
    return arr;
}

/* ============================================================
 * SPOP / SRANDMEMBER
 * ============================================================ */
s_replyObject *cmd_spop(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "ERR syntax");
    uint32_t idx = mredis_hash(args[1]->ptr, args[1]->len) % ((MRedisHeader*)(h->base))->hash_table_size;
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);
    SetHeader *sh = set_get_locked(h, args[1]->ptr, args[1]->len, bk);
    if (!sh || sh->member_count == 0) { pthread_mutex_unlock(&bk->mutex); return reply_nil(); }
    int rc = pthread_mutex_lock(&sh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&sh->mutex);
    pthread_mutex_unlock(&bk->mutex);

    s_replyObject *reply = reply_nil();
    for (uint32_t b = 0; b < sh->n_buckets; b++) {
        uint64_t cur = sh->buckets[b];
        if (cur != OFFSET_NULL) {
            SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
            reply = reply_string(OFF2PTR(h, se->member_offset), se->member_len);
            sh->buckets[b] = se->next_offset;
            heap_free(h, se->member_offset);
            heap_free(h, cur);
            sh->member_count--;
            break;
        }
    }
    pthread_mutex_unlock(&sh->mutex);
    return reply;
}

s_replyObject *cmd_srandmember(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "ERR syntax");
    uint32_t idx = mredis_hash(args[1]->ptr, args[1]->len) % ((MRedisHeader*)(h->base))->hash_table_size;
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);
    SetHeader *sh = set_get_locked(h, args[1]->ptr, args[1]->len, bk);
    if (!sh || sh->member_count == 0) { pthread_mutex_unlock(&bk->mutex); return reply_nil(); }
    int rc = pthread_mutex_lock(&sh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&sh->mutex);
    pthread_mutex_unlock(&bk->mutex);

    s_replyObject *reply = reply_nil();
    for (uint32_t b = 0; b < sh->n_buckets; b++) {
        uint64_t cur = sh->buckets[b];
        if (cur != OFFSET_NULL) {
            SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
            reply = reply_string(OFF2PTR(h, se->member_offset), se->member_len);
            break;
        }
    }
    pthread_mutex_unlock(&sh->mutex);
    return reply;
}

/* ============================================================
 * DROP_SET (완전무결 가비지 컬렉션 구조)
 * ============================================================ */
int drop_set(MRedisHandle *h, const void *key, uint32_t klen)
{
    uint32_t idx = mredis_hash(key, klen) % ((MRedisHeader*)(h->base))->hash_table_size;
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t prev = OFFSET_NULL;
    uint64_t ne_off = bucket_find_locked(h, bk, key, klen, ENTRY_SET, &prev);
    if (ne_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return SHM_ERR_NOT_FOUND;
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

    pthread_mutex_lock(&sh->mutex);
    for (uint32_t b = 0; b < sh->n_buckets; b++) {
        uint64_t cur = sh->buckets[b];
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

    return SHM_OK;
}

/* ============================================================
 * SUNION / SINTER / SDIFF 핵심 구현 (cmd_set.c에 추가)
 * ============================================================ */

/* 복수의 Set 구조체를 안전하게 탐색하기 위한 내부 헬퍼 함수 */
static SetHeader *lock_and_get_set(MRedisHandle *h, const void *key, uint32_t klen)
{
    uint32_t idx = mredis_hash(key, klen) % ((MRedisHeader*)(h->base))->hash_table_size;
    BucketEntry *bk = core_get_bucket(h, idx);

    bucket_lock(h, idx);
    uint64_t prev = OFFSET_NULL;
    uint64_t ne_off = bucket_find_locked(h, bk, key, klen, ENTRY_SET, &prev);
    if (ne_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return NULL;
    }
    NameEntry *ne = (NameEntry *)OFF2PTR(h, ne_off);
    SetHeader *sh = (SetHeader *)OFF2PTR(h, ne->data_offset);

    int rc = pthread_mutex_lock(&sh->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&sh->mutex);

    pthread_mutex_unlock(&bk->mutex);
    return sh;
}

static int set_has_member(MRedisHandle *h, SetHeader *sh, const void *ptr, uint32_t len)
{
    if (!sh) return 0;
    uint32_t bi = mredis_field_hash(ptr, len, sh->n_buckets);
    uint64_t cur = sh->buckets[bi];
    while (cur != OFFSET_NULL) {
        SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
        if (se->member_len == len && memcmp(OFF2PTR(h, se->member_offset), ptr, len) == 0) {
            return 1;
        }
        cur = se->next_offset;
    }
    return 0;
}

/* SUNION key [key ...] */
s_replyObject *cmd_sunion(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: SUNION key [key ...]");

    s_replyObject *result_arr = reply_array(0);

    for (uint32_t i = 1; i < argc; i++) {
        SetHeader *sh = lock_and_get_set(h, args[i]->ptr, args[i]->len);
        if (!sh) continue;

        for (uint32_t b = 0; b < sh->n_buckets; b++) {
            uint64_t cur = sh->buckets[b];
            while (cur != OFFSET_NULL) {
                SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
                void *m_ptr = OFF2PTR(h, se->member_offset);

                // 결과 배열에 중복이 없는지 확인 후 추가 (합집합)
                int duplicate = 0;
                for (uint32_t r = 0; r < result_arr->elements; r++) {
                    s_replyObject *item = result_arr->element[r];
                    if (item->len == se->member_len && memcmp(item->ptr, m_ptr, item->len) == 0) {
                        duplicate = 1;
                        break;
                    }
                }
                if (!duplicate) {
                    reply_array_append(result_arr, reply_string(m_ptr, se->member_len));
                }
                cur = se->next_offset;
            }
        }
        pthread_mutex_unlock(&sh->mutex);
    }
    return result_arr;
}

/* SINTER key [key ...] */
s_replyObject *cmd_sinter(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: SINTER key [key ...]");

    s_replyObject *result_arr = reply_array(0);

    // 첫 번째 Set을 기준으로 잡음
    SetHeader *first_sh = lock_and_get_set(h, args[1]->ptr, args[1]->len);
    if (!first_sh) return result_arr; // 첫 세트가 없으면 교집합은 공집합

    // 나머지 Set 구조체들을 락 및 로드
    SetHeader **other_shs = malloc(sizeof(SetHeader*) * (argc - 2));
    for (uint32_t i = 2; i < argc; i++) {
        other_shs[i - 2] = lock_and_get_set(h, args[i]->ptr, args[i]->len);
    }

    // 첫 번째 Set의 모든 멤버를 돌며 나머지 Set에 모두 존재하는지 확인
    for (uint32_t b = 0; b < first_sh->n_buckets; b++) {
        uint64_t cur = first_sh->buckets[b];
        while (cur != OFFSET_NULL) {
            SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
            void *m_ptr = OFF2PTR(h, se->member_offset);

            int in_all = 1;
            for (uint32_t i = 2; i < argc; i++) {
                if (!other_shs[i - 2] || !set_has_member(h, other_shs[i - 2], m_ptr, se->member_len)) {
                    in_all = 0;
                    break;
                }
            }
            if (in_all) {
                reply_array_append(result_arr, reply_string(m_ptr, se->member_len));
            }
            cur = se->next_offset;
        }
    }

    // 언락 릴리즈
    for (uint32_t i = 2; i < argc; i++) {
        if (other_shs[i - 2]) pthread_mutex_unlock(&other_shs[i - 2]->mutex);
    }
    free(other_shs);
    pthread_mutex_unlock(&first_sh->mutex);

    return result_arr;
}

/* SDIFF key [key ...] */
s_replyObject *cmd_sdiff(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: SDIFF key [key ...]");

    s_replyObject *result_arr = reply_array(0);

    SetHeader *first_sh = lock_and_get_set(h, args[1]->ptr, args[1]->len);
    if (!first_sh) return result_arr;

    SetHeader **other_shs = malloc(sizeof(SetHeader*) * (argc - 2));
    for (uint32_t i = 2; i < argc; i++) {
        other_shs[i - 2] = lock_and_get_set(h, args[i]->ptr, args[i]->len);
    }

    // 첫 번째 Set의 멤버 중 다른 어떤 Set에도 포함되지 않은 것만 필터링 (차집합)
    for (uint32_t b = 0; b < first_sh->n_buckets; b++) {
        uint64_t cur = first_sh->buckets[b];
        while (cur != OFFSET_NULL) {
            SetEntry *se = (SetEntry *)OFF2PTR(h, cur);
            void *m_ptr = OFF2PTR(h, se->member_offset);

            int in_any = 0;
            for (uint32_t i = 2; i < argc; i++) {
                if (other_shs[i - 2] && set_has_member(h, other_shs[i - 2], m_ptr, se->member_len)) {
                    in_any = 1;
                    break;
                }
            }
            if (!in_any) {
                reply_array_append(result_arr, reply_string(m_ptr, se->member_len));
            }
            cur = se->next_offset;
        }
    }

    for (uint32_t i = 2; i < argc; i++) {
        if (other_shs[i - 2]) pthread_mutex_unlock(&other_shs[i - 2]->mutex);
    }
    free(other_shs);
    pthread_mutex_unlock(&first_sh->mutex);

    return result_arr;
}
