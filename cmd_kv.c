/*
 * cmd_kv.c  –  SET / GET / DEL
 *
 *  args[0]=명령어  args[1]=key  args[2]=value(SET 만)
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include "shm_types.h"
#include "shm_core.h"
#include "cmd_kv.h"

/* ── 내부: 버킷 잠근 상태에서 키 타입 확인 ──────────────────
 *   반환:  1  = 새 키 (없음)
 *          0  = 기존 ENTRY_KV (갱신 대상)
 *   SHM_ERR_KEY_EXISTS = 다른 타입으로 존재
 * ─────────────────────────────────────────────────────────── */
static int kv_type_check(ShmHandle *h, BucketEntry *bk,
                          const void *key, uint32_t klen, uint64_t *out_ne)
{
    uint64_t ne = bucket_find_locked(h, bk, key, klen, 0, NULL);
    if (ne == OFFSET_NULL) { if (out_ne) *out_ne = OFFSET_NULL; return 1; }
    NameEntry *nep = (NameEntry *)OFF2PTR(h, ne);
    if (nep->type != ENTRY_KV) return SHM_ERR_KEY_EXISTS;
    if (out_ne) *out_ne = ne;
    return 0;
}

/* ── SET ─────────────────────────────────────────────────────
 *  args: SET key value
 * ─────────────────────────────────────────────────────────── */
s_replyObject *cmd_set(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3)
        return reply_error(SHM_ERR_ARGC, "usage: SET key value");

    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;
    const void *val = args[2]->ptr; uint32_t vlen = args[2]->len;
    if (klen == 0) return reply_error(SHM_ERR_INVAL, "key 비어있음");

    uint32_t idx    = shm_hash(key, klen);
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t ne_off = OFFSET_NULL;
    int chk = kv_type_check(h, bk, key, klen, &ne_off);

    if (chk == SHM_ERR_KEY_EXISTS) {
        pthread_mutex_unlock(&bk->mutex);
        LOG_ERR("SET: '%.*s' 다른 타입 존재", klen, (const char *)key);
        return reply_error(SHM_ERR_KEY_EXISTS, shm_strerror(SHM_ERR_KEY_EXISTS));
    }
    if (chk == 0) {
        /* 기존 KV 값 갱신 */
        NameEntry *ne = (NameEntry *)OFF2PTR(h, ne_off);
        KVNode    *kv = (KVNode *)OFF2PTR(h, ne->data_offset);
        heap_free(h, kv->val_offset);
        uint64_t nvo = heap_alloc(h, vlen > 0 ? vlen : 1);
        if (nvo == OFFSET_NULL) { pthread_mutex_unlock(&bk->mutex); return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM)); }
        if (vlen > 0) memcpy(OFF2PTR(h, nvo), val, vlen);
        kv->val_offset = nvo; kv->val_len = vlen;
        pthread_mutex_unlock(&bk->mutex);
        LOG_TRACE("SET: 갱신 '%.*s'", klen, (const char *)key);
        return reply_ok();
    }
    /* 새 KV 삽입 */
    uint64_t vo     = heap_alloc(h, vlen > 0 ? vlen : 1);
    uint64_t kv_off = heap_alloc(h, sizeof(KVNode));
    if (vo == OFFSET_NULL || kv_off == OFFSET_NULL) {
        if (vo     != OFFSET_NULL) heap_free(h, vo);
        if (kv_off != OFFSET_NULL) heap_free(h, kv_off);
        pthread_mutex_unlock(&bk->mutex);
        return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
    }
    if (vlen > 0) memcpy(OFF2PTR(h, vo), val, vlen);
    KVNode *kv  = (KVNode *)OFF2PTR(h, kv_off);
    kv->val_offset = vo; kv->val_len = vlen; kv->pad = 0;

    uint64_t new_ne = nameentry_alloc(h, key, klen, ENTRY_KV, kv_off);
    if (new_ne == OFFSET_NULL) {
        heap_free(h, kv_off); heap_free(h, vo);
        pthread_mutex_unlock(&bk->mutex);
        return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));
    }
    ((NameEntry *)OFF2PTR(h, new_ne))->next_offset = bk->head_offset;
    bk->head_offset = new_ne;
    pthread_mutex_unlock(&bk->mutex);
    LOG_TRACE("SET: 삽입 '%.*s'", klen, (const char *)key);
    return reply_ok();
}

/* ── GET ─────────────────────────────────────────────────────
 *  args: GET key
 * ─────────────────────────────────────────────────────────── */
s_replyObject *cmd_get(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: GET key");
    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;
    if (klen == 0) return reply_error(SHM_ERR_INVAL, "key 비어있음");

    uint32_t idx    = shm_hash(key, klen);
    BucketEntry *bk = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t ne_off = bucket_find_locked(h, bk, key, klen, ENTRY_KV, NULL);
    if (ne_off == OFFSET_NULL) { pthread_mutex_unlock(&bk->mutex); return reply_nil(); }

    NameEntry *ne = (NameEntry *)OFF2PTR(h, ne_off);
    KVNode    *kv = (KVNode *)OFF2PTR(h, ne->data_offset);
    const char *vp = kv->val_len > 0 ? (const char *)OFF2PTR(h, kv->val_offset) : "";
    s_replyObject *r = reply_string(vp, kv->val_len);
    pthread_mutex_unlock(&bk->mutex);
    return r;
}

/* ============================================================
 *  MSET  key1 value1 key2 value2 ...
 * ============================================================ */
s_replyObject *cmd_mset(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3 || (argc - 1) % 2 != 0)
        return reply_error(SHM_ERR_ARGC, "usage: MSET key1 value1 [key2 value2 ...]");

    int64_t success = 0;

    for (uint32_t i = 1; i + 1 < argc; i += 2) {
        string_t *key_arg = args[i];
        string_t *val_arg = args[i+1];

        string_t *set_args[3] = {
            &STR_LIT("SET"),
            key_arg,
            val_arg
        };

        s_replyObject *r = cmd_set(h, set_args, 3);
        if (r && r->type == REPLY_STATUS) {
            success++;
        }
        reply_free(r);
    }

    return reply_ok();   // Redis는 MSET 성공 시 "OK" 반환
}

/* ============================================================
 *  MGET  key1 key2 key3 ...
 *  반환: ARRAY [value1, value2, value3, ...] (없으면 NIL)
 * ============================================================ */
s_replyObject *cmd_mget(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2)
        return reply_error(SHM_ERR_ARGC, "usage: MGET key1 [key2 ...]");

    s_replyObject *arr = reply_array(argc - 1);
    if (!arr) return reply_error(SHM_ERR_NOMEM, shm_strerror(SHM_ERR_NOMEM));

    for (uint32_t i = 1; i < argc; i++) {
        string_t *get_args[2] = {
            &STR_LIT("GET"),
            args[i]
        };

        s_replyObject *r = cmd_get(h, get_args, 2);
        if (!r) {
            reply_array_append(arr, reply_nil());
        } else {
            reply_array_append(arr, r);   // 소유권 이전
        }
    }

    return arr;
}
/* ── DEL ─────────────────────────────────────────────────────
 *  args: DEL key [key …]
 *  반환: INTEGER 삭제 수
 * ─────────────────────────────────────────────────────────── */
s_replyObject *cmd_del(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: DEL key [key …]");

    int64_t removed = 0;
    for (uint32_t a = 1; a < argc; a++) {
        const void *key = args[a]->ptr; uint32_t klen = args[a]->len;
        if (klen == 0) continue;

        uint32_t idx    = shm_hash(key, klen);
        BucketEntry *bk = core_get_bucket(h, idx);
        bucket_lock(h, idx);

        uint64_t prev   = OFFSET_NULL;
        uint64_t ne_off = bucket_find_locked(h, bk, key, klen, ENTRY_KV, &prev);
        if (ne_off == OFFSET_NULL) { pthread_mutex_unlock(&bk->mutex); continue; }

        NameEntry *ne = (NameEntry *)OFF2PTR(h, ne_off);
        KVNode    *kv = (KVNode *)OFF2PTR(h, ne->data_offset);

        if (prev == OFFSET_NULL) bk->head_offset = ne->next_offset;
        else ((NameEntry *)OFF2PTR(h, prev))->next_offset = ne->next_offset;

        heap_free(h, kv->val_offset);
        heap_free(h, ne->data_offset);
        nameentry_free(h, ne_off);
        pthread_mutex_unlock(&bk->mutex);
        removed++;
        LOG_TRACE("DEL: '%.*s'", klen, (const char *)key);
    }
    return reply_integer(removed);
}
