#ifndef SHM_CORE_H
#define SHM_CORE_H

#include "shm_types.h"

/* ============================================================
 *  로그 매크로
 * ============================================================ */
extern int g_shm_dbg;
#define LOG_ERR(...)   do{ if(g_shm_dbg>=DBG_ERROR){fprintf(stderr,"SHM [ERROR] ");fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
#define LOG_WARN(...)  do{ if(g_shm_dbg>=DBG_WARN) {fprintf(stderr,"SHM [WARN]  ");fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
#define LOG_INFO(...)  do{ if(g_shm_dbg>=DBG_INFO) {fprintf(stderr,"SHM [INFO]  ");fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)
#define LOG_TRACE(...) do{ if(g_shm_dbg>=DBG_TRACE){fprintf(stderr,"SHM [TRACE] ");fprintf(stderr,__VA_ARGS__);fputc('\n',stderr);}}while(0)

/* ============================================================
 *  오프셋 ↔ 포인터
 * ============================================================ */
#define SHM_BASE(h)      ((uint8_t*)(h)->base)
#define OFF2PTR(h,off)   ((void*)(SHM_BASE(h)+(off)))
#define PTR2OFF(h,ptr)   ((uint64_t)((uint8_t*)(ptr)-SHM_BASE(h)))

/* ============================================================
 *  내부 구조체 접근자
 * ============================================================ */
static inline ShmHeader   *core_shm_hdr(ShmHandle *h)   { return (ShmHeader*)SHM_BASE(h); }
static inline BucketEntry *core_get_bucket(ShmHandle *h, uint32_t i)
    { return (BucketEntry*)(SHM_BASE(h)+core_shm_hdr(h)->bucket_offset)+i; }
static inline HeapHeader  *core_heap_hdr(ShmHandle *h)
    { return (HeapHeader*)(SHM_BASE(h)+core_shm_hdr(h)->heap_header_offset); }
static inline uint64_t    *hh_field_buckets(HashHeader *hh)
    { return (uint64_t*)((uint8_t*)hh+sizeof(HashHeader)); }
static inline SkipNode    *core_sn(ShmHandle *h, uint64_t off)
    { return (SkipNode*)OFF2PTR(h,off); }

#if 0
static inline FieldPoolHeader *get_field_pool(ShmHandle *h)	{
	ShmHeader* s = (ShmHeader*)h->base;
	return &s->field_pool;
#if 0
	if (s->field_pool_offset == OFFSET_NULL)	return NULL;
	return OFF2PTR(h,s->field_pool_offset);
#endif
}
#endif
/* ============================================================
 *  SHM 생명주기
 * ============================================================ */
ShmHandle  *shm_create(const char *name, uint64_t size);
ShmHandle  *shm_open_existing(const char *name);
void        shm_close(ShmHandle *h);
void        shm_destroy(const char *name);
void        shm_set_debug_level(int level);
void        shm_dump_stats(ShmHandle *h);
const char *shm_strerror(int err);

/* ============================================================
 *  해시 함수
 * ============================================================ */
uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint32_t shm_hash(const void *key, uint32_t klen);
uint32_t shm_field_hash(const void *f, uint32_t flen, uint32_t nbuckets);

/* ============================================================
 *  힙
 * ============================================================ */
uint64_t heap_alloc(ShmHandle *h, uint64_t size);
int      heap_free(ShmHandle *h, uint64_t off);

/* ============================================================
 *  버킷 헬퍼
 * ============================================================ */
void     bucket_lock(ShmHandle *h, uint32_t idx);
uint64_t bucket_find_locked(ShmHandle *h, BucketEntry *bk,
                              const void *key, uint32_t klen,
                              uint32_t type_filter, uint64_t *out_prev);
uint32_t bucket_find_entry_number(ShmHandle *h, BucketEntry *bk,
                              const void *key, uint32_t klen);
uint64_t bucket_find(ShmHandle *h, uint32_t idx,
                      const void *key, uint32_t klen,
                      uint32_t type_filter, uint64_t *out_prev);

int type_check(ShmHandle *h, BucketEntry *bk, const void *key, uint32_t klen, uint32_t type, uint64_t *out_ne);
/* ============================================================
 *  NameEntry
 * ============================================================ */
uint64_t nameentry_alloc(ShmHandle *h, const void *key, uint32_t klen,
                          uint32_t type, uint64_t data_off);
void     nameentry_free(ShmHandle *h, uint64_t ne_off);

void	pthread_mutex_initialize(pthread_mutex_t *p_mutex);
/* ============================================================
 *  ZSetHeader / HashHeader 조회
 * ============================================================ */
ZSetHeader *core_zset_get(ShmHandle *h, const void *name, uint32_t nlen);
HashHeader *core_hash_get(ShmHandle *h, const void *key,  uint32_t klen);

/* ============================================================
 *  Skip List
 * ============================================================ */
uint32_t sl_random_level(void);
int      sl_cmp(double s1,const void*m1,uint32_t ml1,double s2,const void*m2,uint32_t ml2);
uint64_t sl_find_update(ShmHandle*h,ZSetHeader*z,double sc,const void*m,uint32_t ml,uint64_t upd[ZSET_MAX_LEVEL]);
uint64_t sl_find_member(ShmHandle*h,ZSetHeader*z,const void*m,uint32_t ml);
uint64_t sl_node_alloc(ShmHandle*h,double sc,const void*m,uint32_t ml,uint32_t lv);
void     sl_node_free(ShmHandle*h,uint64_t n);
void     sl_unlink(ShmHandle*h,ZSetHeader*z,uint64_t n,uint64_t upd[ZSET_MAX_LEVEL]);

#endif /* SHM_CORE_H */
