/*
 * cmd_kv.c  –  SET / GET
 *
 * 버그 수정:
 *  [FIX-1] cmd_set 갱신 경로: heap_free(old_val) 후 heap_alloc 실패 시
 *          kv->val_offset 가 이미 해제된 오프셋을 가리킴.
 *          → 새 값 할당 성공 후에 old 해제 순서로 변경.
 *  (나머지 경로는 원본 정상)
 */
#include <stdlib.h>
#include <string.h>
#include "mredis_types.h"
#include "mredis_core.h"
#include "cmd_kv.h"

static int kv_type_check(MRedisHandle *h, BucketEntry *bk,
                          const void *key, uint32_t klen, uint64_t *out_ne)
{
    uint64_t ne = bucket_find_locked(h, bk, key, klen, 0, NULL);
    if (ne == OFFSET_NULL) { if (out_ne) *out_ne = OFFSET_NULL; return 1; }
    NameEntry *nep = (NameEntry *)OFF2PTR(h, ne);
    if (nep->type != ENTRY_KV) return SHM_ERR_KEY_EXISTS;
    if (out_ne) *out_ne = ne;
    return 0;
}

/* ── SET ─────────────────────────────────────────────────── */
s_replyObject *cmd_set(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3) return reply_error(SHM_ERR_ARGC, "usage: SET key value");
    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;
    const void *val = args[2]->ptr; uint32_t vlen = args[2]->len;
    if (klen == 0) return reply_error(SHM_ERR_INVAL, "key 비어있음");

    uint32_t     idx = mredis_hash(key, klen)%((MRedisHeader*)(h->base))->hash_table_size;
    BucketEntry *bk  = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t ne_off = OFFSET_NULL;
    int chk = kv_type_check(h, bk, key, klen, &ne_off);

    if (chk == SHM_ERR_KEY_EXISTS) {
        pthread_mutex_unlock(&bk->mutex);
        return reply_error(SHM_ERR_KEY_EXISTS, mredis_strerror(SHM_ERR_KEY_EXISTS));
    }
    if (chk == 0) {
        /* [FIX-1] 새 값 먼저 할당, 성공 후 old 해제 */
        NameEntry *ne  = (NameEntry *)OFF2PTR(h, ne_off);
        KVNode    *kv  = (KVNode *)OFF2PTR(h, ne->data_offset);
        uint64_t   nvo = heap_alloc(h, vlen > 0 ? vlen : 1);
        if (nvo == OFFSET_NULL) {
            pthread_mutex_unlock(&bk->mutex);
            return reply_error(SHM_ERR_NOMEM, mredis_strerror(SHM_ERR_NOMEM));
        }
        if (vlen > 0) memcpy(OFF2PTR(h, nvo), val, vlen);
        heap_free(h, kv->val_offset);   /* old 해제는 new 성공 후 */
        kv->val_offset = nvo;
        kv->val_len    = vlen;
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
        return reply_error(SHM_ERR_NOMEM, mredis_strerror(SHM_ERR_NOMEM));
    }
    if (vlen > 0) memcpy(OFF2PTR(h, vo), val, vlen);
    KVNode *kv  = (KVNode *)OFF2PTR(h, kv_off);
    kv->val_offset = vo; kv->val_len = vlen; kv->pad = 0;

    uint64_t new_ne = nameentry_alloc(h, key, klen, ENTRY_KV, kv_off);
    if (new_ne == OFFSET_NULL) {
        heap_free(h, kv_off); heap_free(h, vo);
        pthread_mutex_unlock(&bk->mutex);
        return reply_error(SHM_ERR_NOMEM, mredis_strerror(SHM_ERR_NOMEM));
    }
    ((NameEntry *)OFF2PTR(h, new_ne))->next_offset = bk->head_offset;
    bk->head_offset = new_ne;
    pthread_mutex_unlock(&bk->mutex);
    LOG_TRACE("SET: 삽입 '%.*s'", klen, (const char *)key);
    return reply_ok();
}

/* ── GET ─────────────────────────────────────────────────── */
s_replyObject *cmd_get(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: GET key");
    const void *key = args[1]->ptr; uint32_t klen = args[1]->len;
    if (klen == 0) return reply_error(SHM_ERR_INVAL, "key 비어있음");

    uint32_t     idx = mredis_hash(key, klen)%((MRedisHeader*)(h->base))->hash_table_size;
    BucketEntry *bk  = core_get_bucket(h, idx);
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

/* ── ENTRY_KV drop ──────────────────────────────────────────
 *  bucket 을 lock 한 상태에서 NameEntry 를 bucket chain 에서
 *  제거하고 KVNode + value 를 힙에서 해제한다.
 * ─────────────────────────────────────────────────────────── */
int drop_kv(MRedisHandle *h, const void *key, uint32_t klen)
{
    uint32_t     idx  = mredis_hash(key, klen)%((MRedisHeader*)(h->base))->hash_table_size;
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
