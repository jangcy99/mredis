#ifndef SHM_CORE_H
#define SHM_CORE_H

#include "mredis_types.h"

/* ============================================================
 *  로그 매크로
 * ============================================================ */
extern int g_mredis_dbg;
#define LOG_ERR(...)   do{ if(g_mredis_dbg>=DBG_ERROR){fprintf(stderr,"SHM [ERROR] ");fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
#define LOG_WARN(...)  do{ if(g_mredis_dbg>=DBG_WARN) {fprintf(stderr,"SHM [WARN]  ");fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
#define LOG_INFO(...)  do{ if(g_mredis_dbg>=DBG_INFO) {fprintf(stderr,"SHM [INFO]  ");fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
#define LOG_TRACE(...) do{ if(g_mredis_dbg>=DBG_TRACE){fprintf(stderr,"SHM [TRACE] ");fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)

/* ============================================================
 *  오프셋 ↔ 포인터
 * ============================================================ */
#define SHM_BASE(h)      ((uint8_t*)(h)->base)
#define OFF2PTR(h,off)   ((void*)(SHM_BASE(h)+(off)))
#define PTR2OFF(h,ptr)   ((uint64_t)((uint8_t*)(ptr)-SHM_BASE(h)))

/* ============================================================
 *  내부 구조체 접근자
 * ============================================================ */
static inline MRedisHeader   *core_mredis_hdr(MRedisHandle *h)   { return (MRedisHeader*)SHM_BASE(h); }
static inline BucketEntry *core_get_bucket(MRedisHandle *h, uint32_t i)
    { return (BucketEntry*)(SHM_BASE(h)+core_mredis_hdr(h)->bucket_offset)+i; }
static inline HeapHeader  *core_heap_hdr(MRedisHandle *h)
    { return (HeapHeader*)(SHM_BASE(h)+core_mredis_hdr(h)->heap_header_offset); }

/* ============================================================
 *  SHM 생명주기
 * ============================================================ */
MRedisHandle  *mredis_create(const char *name, uint64_t size);
MRedisHandle  *mredis_open_existing(const char *name);
void        mredis_close(MRedisHandle *h);
void        mredis_destroy(const char *name);
void        mredis_set_debug_level(int level);
void        mredis_dump_stats(MRedisHandle *h);
const char *mredis_strerror(int err);

/* ============================================================
 *  해시 함수
 * ============================================================ */
uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t mredis_hash(const void *key, uint32_t klen);
uint32_t mredis_field_hash(const void *f, uint32_t flen, uint32_t nbuckets);

/* ============================================================
 *  힙
 * ============================================================ */
uint64_t heap_alloc(MRedisHandle *h, uint64_t size);
int      heap_free(MRedisHandle *h, uint64_t off);

/* ============================================================
 *  버킷 헬퍼
 * ============================================================ */
void     bucket_lock(MRedisHandle *h, uint32_t idx);
uint64_t bucket_find_locked(MRedisHandle *h, BucketEntry *bk,
                              const void *key, uint32_t klen,
                              uint32_t type_filter, uint64_t *out_prev);
uint32_t bucket_find_entry_number(MRedisHandle *h, BucketEntry *bk,
                              const void *key, uint32_t klen);
uint64_t bucket_find(MRedisHandle *h, uint32_t idx,
                      const void *key, uint32_t klen,
                      uint32_t type_filter, uint64_t *out_prev);

int type_check(MRedisHandle *h, BucketEntry *bk, const void *key, uint32_t klen, uint32_t type, uint64_t *out_ne);
/* ============================================================
 *  NameEntry
 * ============================================================ */
uint64_t nameentry_alloc(MRedisHandle *h, const void *key, uint32_t klen,
                          uint32_t type, uint64_t data_off);
void     nameentry_free(MRedisHandle *h, uint64_t ne_off);

void	pthread_mutex_initialize(pthread_mutex_t *p_mutex);

#endif /* SHM_CORE_H */
