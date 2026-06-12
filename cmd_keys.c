/*
 * cmd_keys.c  –  KEYS pattern
 *
 * 버그 수정:
 *  [FIX-1] 각 버킷을 순회할 때 bucket->mutex 를 잠그지 않아
 *          다른 프로세스의 동시 삽입/삭제와 경쟁 조건 발생.
 *          → 버킷별 bucket_lock / unlock 추가.
 *  [FIX-2] 에러 메시지가 "usage: SET key value" 로 잘못됨.
 *          → "usage: KEYS pattern" 으로 수정.
 */
#include <stdlib.h>
#include <string.h>
#include <fnmatch.h>
#include "mredis_types.h"
#include "mredis_core.h"
#include "cmd_keys.h"

s_replyObject *cmd_keys(MRedisHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2)
        return reply_error(SHM_ERR_ARGC, "usage: KEYS pattern");

    const char *pattern = args[1]->ptr;
    MRedisHeader  *s       = (MRedisHeader *)h->base;
    s_replyObject *arr  = reply_array(0);
    if (!arr) return reply_error(SHM_ERR_NOMEM, "메모리 부족");

    for (uint32_t i = 0; i < s->hash_table_size; i++) {
        BucketEntry *bk = core_get_bucket(h, i);

        /* [FIX-1] 버킷 잠금으로 동시 수정 보호 */
        bucket_lock(h, i);

        if (bk->head_offset == OFFSET_NULL) {
            pthread_mutex_unlock(&bk->mutex);
            continue;
        }
        uint64_t offset = bk->head_offset;
        while (offset != OFFSET_NULL) {
            NameEntry *entry = (NameEntry *)OFF2PTR(h, offset);
            if (fnmatch(pattern,
                        (const char *)OFF2PTR(h, entry->key_offset), 0) == 0) {
                s_replyObject *item = reply_string(
                    (const char *)OFF2PTR(h, entry->key_offset),
                    entry->key_len);
                if (item) reply_array_append(arr, item);
            }
            offset = entry->next_offset;
        }
        pthread_mutex_unlock(&bk->mutex);
    }
    return arr;
}
