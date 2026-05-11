/*
 * shm_core.c  –  SHM 엔진 (힙, 버킷, Skip List, 생명주기)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include "shm_types.h"
#include "shm_core.h"

int g_shm_dbg = SHM_DEBUG_LEVEL;
void shm_set_debug_level(int l) { g_shm_dbg = l; }

const char *shm_strerror(int e)
{
    switch(e){
    case SHM_OK:            return "OK";
    case SHM_ERR:           return "일반 에러";
    case SHM_ERR_KEY_EXISTS:return "키가 다른 타입으로 이미 존재";
    case SHM_ERR_NOT_FOUND: return "키/필드 없음";
    case SHM_ERR_NOMEM:     return "메모리 부족";
    case SHM_ERR_INVAL:     return "잘못된 인자";
    case SHM_ERR_OVERFLOW:  return "크기 overflow";
    case SHM_ERR_CORRUPT:   return "메모리 손상";
    case SHM_ERR_TYPE:      return "타입 불일치";
    case SHM_ERR_ARGC:      return "인자 수 오류";
    case SHM_ERR_PARSE:     return "파싱 실패";
    default:                return "알 수 없는 에러";
    }
}

/* ──────────────────── 해시 ──────────────────────────────── */
uint32_t shm_hash(const void *key, uint32_t klen)
{
	uint8_t	keydef[] = "0123456789012345";
	return (uint32_t)siphash (key, klen, keydef) % (uint64_t)HASH_TABLE_SIZE;
#if 0
    const uint8_t *p = (const uint8_t *)key;
    uint64_t h = 14695981039346656037ULL;
    for (uint32_t i = 0; i < klen; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return (uint32_t)(h % (uint64_t)HASH_TABLE_SIZE);
#endif
}
uint32_t shm_field_hash(const void *f, uint32_t flen, uint32_t nb)
{
	uint8_t	keydef[] = "0123456789012345";
	return (uint32_t)siphash (f, flen, keydef) % (uint64_t)nb;
#if 0
    const uint8_t *p = (const uint8_t *)f;
    uint64_t h = 14695981039346656037ULL;
    for (uint32_t i = 0; i < flen; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return (uint32_t)(h % (uint64_t)nb);
#endif
}

/* ============================================================
 *  Best-Fit Bin Heap (24-byte BlockHeader)
 * ============================================================ */

#define ALIGN_SIZE         8
#define MIN_BLOCK_SIZE     64

/* Flags 조작 매크로 */
#define GET_IS_FREE(f)     ((f) & 1u)
#define SET_IS_FREE(f, v)  do { (f) = ((f) & ~1u) | ((v) ? 1u : 0u); } while(0)

#define GET_BIN_IDX(f)     (((f) >> 8) & 0xFFFFFFu)
#define SET_BIN_IDX(f, b)  do { (f) = ((f) & 0xFFu) | (((uint32_t)(b) & 0xFFFFFFu) << 8); } while(0)

static inline uint32_t get_bin_index(uint64_t size)
{
    if (size < 512) return (uint32_t)(size / 16);
    uint32_t bin = 32;
    uint64_t s = size >> 8;
    while (s > 1) { s >>= 1; bin++; }
    return bin < BIN_COUNT ? bin : BIN_COUNT - 1;
}

/* ============================================================
 *  heap_alloc_locked
 * ============================================================ */
static uint64_t heap_alloc_locked(ShmHandle *h, uint64_t req)
{
    if (req == 0) req = 8;
    uint64_t need = req + sizeof(BlockHeader);
    need = (need + ALIGN_SIZE - 1) & ~(ALIGN_SIZE - 1);
    if (need < MIN_BLOCK_SIZE) need = MIN_BLOCK_SIZE;

    HeapHeader *hh = core_heap_hdr(h);
    uint32_t start_bin = get_bin_index(need);

    for (uint32_t b = start_bin; b < BIN_COUNT; b++) {
        uint64_t cur = hh->free_bins[b];
        uint64_t best = OFFSET_NULL;
        uint64_t best_size = UINT64_MAX;
        uint64_t best_prev = OFFSET_NULL;
        uint64_t prev = OFFSET_NULL;

        while (cur != OFFSET_NULL) {
            BlockHeader *bh = (BlockHeader *)OFF2PTR(h, cur);
            if (bh->magic != HEAP_BLOCK_MAGIC || !GET_IS_FREE(bh->flags)) {
                prev = cur;
                cur = bh->next;
                continue;
            }

            if (bh->size >= need && bh->size < best_size) {
                best_size = bh->size;
                best = cur;
                best_prev = prev;
            }
            prev = cur;
            cur = bh->next;
        }

        if (best != OFFSET_NULL) {
            BlockHeader *bh = (BlockHeader *)OFF2PTR(h, best);

            /* free list에서 제거 */
            if (best_prev == OFFSET_NULL)
                hh->free_bins[b] = bh->next;
            else
                ((BlockHeader*)OFF2PTR(h, best_prev))->next = bh->next;

            uint64_t rem = bh->size - need;

            if (rem >= MIN_BLOCK_SIZE) {
                uint64_t split_off = best + need;
                BlockHeader *split = (BlockHeader *)OFF2PTR(h, split_off);
                split->size  = rem - sizeof(BlockHeader);
                split->magic = HEAP_BLOCK_MAGIC;
                split->next  = OFFSET_NULL;
                SET_IS_FREE(split->flags, 1);
                SET_BIN_IDX(split->flags, get_bin_index(split->size));

                uint32_t sbin = GET_BIN_IDX(split->flags);
                split->next = hh->free_bins[sbin];
                hh->free_bins[sbin] = split_off;
            } else {
                need = bh->size;
            }

            /* 할당 */
            bh->size = need - sizeof(BlockHeader);
            SET_IS_FREE(bh->flags, 0);
            bh->next = 0;

            hh->used_bytes += need;
            hh->total_alloc++;
            return best + sizeof(BlockHeader);
        }
    }

    LOG_ERR("heap_alloc:(%d) Out of memory", getpid());
    return OFFSET_NULL;
}

/* ============================================================
 *  heap_free_locked
 * ============================================================ */
static int heap_free_locked(ShmHandle *h, uint64_t data_off)
{
    if (data_off == OFFSET_NULL) return SHM_OK;

    HeapHeader *hh = core_heap_hdr(h);
    uint64_t block_off = data_off - sizeof(BlockHeader);
    BlockHeader *bh = (BlockHeader *)OFF2PTR(h, block_off);

    if (bh->magic != HEAP_BLOCK_MAGIC) {
        LOG_ERR("heap_free: Invalid magic at 0x%lx", block_off);
        return SHM_ERR_CORRUPT;
    }
    if (GET_IS_FREE(bh->flags)) {
        LOG_WARN("Double free at 0x%lx", block_off);
        return SHM_ERR;
    }

    uint64_t freed_size = bh->size + sizeof(BlockHeader);

    SET_IS_FREE(bh->flags, 1);
    SET_BIN_IDX(bh->flags, get_bin_index(bh->size));

    uint32_t bin = GET_BIN_IDX(bh->flags);
    bh->next = hh->free_bins[bin];
    hh->free_bins[bin] = block_off;

    if (hh->used_bytes >= freed_size) {
        hh->used_bytes -= freed_size;
    } else {
        LOG_WARN("used_bytes underflow! (current=%lu, freed=%lu) → reset", hh->used_bytes, freed_size);
        hh->used_bytes = 0;
    }

    hh->total_free++;
    return SHM_OK;
}

/* ============================================================
 *  Public Functions
 * ============================================================ */
uint64_t heap_alloc(ShmHandle *h, uint64_t size)
{
    HeapHeader *hh = core_heap_hdr(h);
    int rc = pthread_mutex_lock(&hh->heap_mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&hh->heap_mutex);
    uint64_t off = heap_alloc_locked(h, size);
    pthread_mutex_unlock(&hh->heap_mutex);
    return off;
}

int heap_free(ShmHandle *h, uint64_t data)
{
    if (data == OFFSET_NULL) return SHM_OK;
    HeapHeader *hh = core_heap_hdr(h);
    int rc = pthread_mutex_lock(&hh->heap_mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&hh->heap_mutex);
    int ret = heap_free_locked(h, data);
    pthread_mutex_unlock(&hh->heap_mutex);
    return ret;
}

/* ──────────────────── 레이아웃 / 생명주기 ──────────────── */
static void calc_layout(uint64_t sz, uint64_t *ob, uint64_t *ohh, uint64_t *ohs, uint64_t *ohsz)
{
    uint64_t off = (sizeof(ShmHeader) + 7ULL) & ~7ULL;
    *ob = off; off += (uint64_t)HASH_TABLE_SIZE * sizeof(BucketEntry); off = (off + 7ULL) & ~7ULL;
    *ohh = off; off += sizeof(HeapHeader); off = (off + 7ULL) & ~7ULL;
    *ohs = off; *ohsz = (off < sz) ? (sz - off) : 0;
    LOG_INFO("레이아웃: bucket=0x%lx heap_hdr=0x%lx heap=%lu MB", *ob, *ohh, (*ohsz)>>20);
}

static void heap_init_region(ShmHandle *h, uint64_t hs, uint64_t hsz)
{
    HeapHeader *hh = core_heap_hdr(h);

    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&hh->heap_mutex, &a);
    pthread_mutexattr_destroy(&a);

    for (int i = 0; i < BIN_COUNT; i++)
        hh->free_bins[i] = OFFSET_NULL;

    hh->heap_start = hs;
    hh->heap_size = hsz;
    hh->total_alloc = hh->total_free = 0;
    hh->used_bytes = 0;

    BlockHeader *b = (BlockHeader *)OFF2PTR(h, hs);
    b->size = hsz - sizeof(BlockHeader);
    b->magic = HEAP_BLOCK_MAGIC;
    b->next = OFFSET_NULL;
    SET_IS_FREE(b->flags, 1);
    SET_BIN_IDX(b->flags, get_bin_index(b->size));

    hh->free_bins[GET_BIN_IDX(b->flags)] = hs;

    LOG_INFO("Best-Fit Bin Heap 초기화 완료 (%lu MB)", hsz >> 20);
}

static int32_t create_field_pool(ShmHandle *h)
{
    ShmHeader *s = (ShmHeader *)h->base;

#if 0
    s->field_pool_offset = heap_alloc(h, sizeof(FieldPoolHeader));
    if (s->field_pool_offset == OFFSET_NULL) return -1;
#endif

    FieldPoolHeader *fp = (FieldPoolHeader *)&s->field_pool;
	for (int i = 0; i < 256; i ++)	fp->field_buckets[i] = OFFSET_NULL;
    fp->total_fields = 0;

    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(&fp->mutex, &ma);
    pthread_mutexattr_destroy(&ma);

    return 0;
}

ShmHandle *shm_create(const char *name, uint64_t size)
{
    LOG_INFO("shm_create: %s %lu MB", name, size>>20);
    if (size < (16ULL<<20)) { size = 16ULL<<20; }
    int fd = shm_open(name, O_CREAT|O_EXCL|O_RDWR, 0666);
    if (fd < 0) { LOG_ERR("shm_open: %s", strerror(errno)); return NULL; }
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); shm_unlink(name); return NULL; }
    void *base = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { close(fd); shm_unlink(name); return NULL; }
    memset(base, 0, size);
    ShmHandle *h = (ShmHandle *)malloc(sizeof(ShmHandle));
    if (!h) { munmap(base, size); close(fd); shm_unlink(name); return NULL; }
    h->base = base; h->fd = fd; h->size = size;
    uint64_t bo, hho, hs, hsz; calc_layout(size, &bo, &hho, &hs, &hsz);
    if (!hsz) { free(h); munmap(base,size); close(fd); shm_unlink(name); return NULL; }
    ShmHeader *s = (ShmHeader *)base;
    s->shm_size = size; s->bucket_offset = bo; s->heap_header_offset = hho;
    s->hash_table_size = (uint32_t)HASH_TABLE_SIZE; s->version = 5; s->initialized = 0;
    pthread_mutexattr_t ma; pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&ma, PTHREAD_MUTEX_ROBUST);
    BucketEntry *bk = (BucketEntry *)((uint8_t *)base + bo);
    for (uint32_t i = 0; i < (uint32_t)HASH_TABLE_SIZE; i++) {
        bk[i].head_offset = OFFSET_NULL; pthread_mutex_init(&bk[i].mutex, &ma);
        if (i > 0 && (i & 0xFFFFF) == 0) LOG_TRACE("뮤텍스 %u/%u", i, (uint32_t)HASH_TABLE_SIZE);
    }
    pthread_mutexattr_destroy(&ma);
    heap_init_region(h, hs, hsz);
	create_field_pool(h);
    s->initialized = 1;
    LOG_INFO("shm_create 완료 ver=%u", s->version);
    return h;
}
ShmHandle *shm_open_existing(const char *name)
{
    int fd = shm_open(name, O_RDWR, 0666);
    if (fd < 0) { LOG_ERR("shm_open: %s", strerror(errno)); return NULL; }
    struct stat st; fstat(fd, &st); uint64_t size = (uint64_t)st.st_size;
    void *base = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { close(fd); return NULL; }
    if (!((ShmHeader *)base)->initialized) { munmap(base,size); close(fd); return NULL; }
    ShmHandle *h = (ShmHandle *)malloc(sizeof(ShmHandle));
    if (!h) { munmap(base,size); close(fd); return NULL; }
    h->base = base; h->fd = fd; h->size = size;
    LOG_INFO("shm_open_existing: %s %lu MB", name, size>>20);
    return h;
}
void shm_close(ShmHandle *h)  { if(h){ munmap(h->base,h->size); close(h->fd); free(h); } }
void shm_destroy(const char *name) { shm_unlink(name); }
void shm_dump_stats(ShmHandle *h)
{
    if (!h) return;
    ShmHeader *s = core_shm_hdr(h); HeapHeader *hh = core_heap_hdr(h);
    fprintf(stderr,"=== SHM 통계 (ver%u) ===\n  SHM=%lu MB  버킷=0x%x\n"
            "  힙=%lu MB  사용=%lu KB  잔여=%lu MB\n  alloc/free=%lu/%lu\n",
            s->version, s->shm_size>>20, s->hash_table_size,
            hh->heap_size>>20, hh->used_bytes>>10, (hh->heap_size-hh->used_bytes)>>20,
            hh->total_alloc, hh->total_free);
}

/* ──────────────────── 버킷 ──────────────────────────────── */
void bucket_lock(ShmHandle *h, uint32_t idx)
{
    BucketEntry *bk = core_get_bucket(h, idx);
    int rc = pthread_mutex_lock(&bk->mutex);
    if (rc == EOWNERDEAD) { LOG_WARN("버킷[0x%x] mutex 복구", idx); pthread_mutex_consistent(&bk->mutex); }
}
uint64_t bucket_find_locked(ShmHandle *h, BucketEntry *bk,
                              const void *key, uint32_t klen,
                              uint32_t tf, uint64_t *op)
{
    uint64_t prev = OFFSET_NULL, cur = bk->head_offset;
    while (cur != OFFSET_NULL) {
        NameEntry *ne = (NameEntry *)OFF2PTR(h, cur);
        if (ne->key_len == klen && memcmp(OFF2PTR(h, ne->key_offset), key, klen) == 0
            && (tf == 0 || ne->type == tf)) { if (op) *op = prev; return cur; }
        prev = cur; cur = ne->next_offset;
    }
    if (op) *op = prev;
	return OFFSET_NULL;
}
uint64_t bucket_find(ShmHandle *h, uint32_t idx, const void *key, uint32_t klen,
                      uint32_t tf, uint64_t *op)
{
    bucket_lock(h, idx);
    BucketEntry *bk = core_get_bucket(h, idx);
    uint64_t ne = bucket_find_locked(h, bk, key, klen, tf, op);
    pthread_mutex_unlock(&bk->mutex);
    return ne;
}

/* ──────────────────── NameEntry ─────────────────────────── */
uint64_t nameentry_alloc(ShmHandle *h, const void *key, uint32_t klen,
                          uint32_t type, uint64_t data_off)
{
    uint64_t ne_off = heap_alloc(h, sizeof(NameEntry));
    uint64_t ko     = heap_alloc(h, klen);
    if (ne_off == OFFSET_NULL || ko == OFFSET_NULL) {
        if (ne_off != OFFSET_NULL) heap_free(h, ne_off);
        if (ko     != OFFSET_NULL) heap_free(h, ko);
        return OFFSET_NULL;
    }
    memcpy(OFF2PTR(h, ko), key, klen);
    NameEntry *ne = (NameEntry *)OFF2PTR(h, ne_off);
    ne->next_offset = OFFSET_NULL; ne->key_offset = ko;
    ne->key_len = klen; ne->type = type; ne->data_offset = data_off;
    return ne_off;
}
void nameentry_free(ShmHandle *h, uint64_t ne_off)
{
    if (ne_off == OFFSET_NULL) return;
    NameEntry *ne = (NameEntry *)OFF2PTR(h, ne_off);
    heap_free(h, ne->key_offset); heap_free(h, ne_off);
}

/* ──────────────────── ZSet / Hash 조회 ──────────────────── */
ZSetHeader *core_zset_get(ShmHandle *h, const void *name, uint32_t nlen)
{
    uint64_t ne = bucket_find(h, shm_hash(name,nlen), name, nlen, ENTRY_ZSET, NULL);
    if (ne == OFFSET_NULL) return NULL;
    return (ZSetHeader *)OFF2PTR(h, ((NameEntry *)OFF2PTR(h,ne))->data_offset);
}
HashHeader *core_hash_get(ShmHandle *h, const void *key, uint32_t klen)
{
    uint64_t ne = bucket_find(h, shm_hash(key,klen), key, klen, ENTRY_HASH, NULL);
    if (ne == OFFSET_NULL) return NULL;
    return (HashHeader *)OFF2PTR(h, ((NameEntry *)OFF2PTR(h,ne))->data_offset);
}

/* ──────────────────── Skip List ─────────────────────────── */
uint32_t sl_random_level(void)
{
    static __thread uint32_t rng = 0;
    if (!rng) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); rng=(uint32_t)(ts.tv_nsec^(uintptr_t)&rng)|1u; }
    uint32_t lv = 1;
    while (lv < ZSET_MAX_LEVEL) { rng^=rng<<13;rng^=rng>>17;rng^=rng<<5; if((rng&0xFFFF)<(uint32_t)(0xFFFF*ZSET_P))lv++; else break; }
    return lv;
}
int sl_cmp(double s1,const void*m1,uint32_t ml1,double s2,const void*m2,uint32_t ml2)
{
    if(s1<s2)return -1;
	if(s1>s2)return 1;
    uint32_t mn=ml1<ml2?ml1:ml2; int r=(m1&&m2)?memcmp(m1,m2,mn):0; if(r)return r;
    return (ml1<ml2)?-1:(ml1>ml2)?1:0;
}
uint64_t sl_find_update(ShmHandle *h,ZSetHeader *zsh,double sc,const void *m,uint32_t ml,uint64_t upd[ZSET_MAX_LEVEL])
{
    uint64_t x=zsh->head_offset;
    for(int i=(int)zsh->cur_level-1;i>=0;i--){
        SkipNode *sn=core_sn(h,x);
        while(sn->forward[i]!=OFFSET_NULL){
            SkipNode *nx=core_sn(h,sn->forward[i]);
            void *nm=(nx->member_offset!=OFFSET_NULL)?OFF2PTR(h,nx->member_offset):NULL;
            if(sl_cmp(nx->score,nm,nx->member_len,sc,m,ml)<0){x=sn->forward[i];sn=nx;}else break;
        }
        upd[i]=x;
    }
    return core_sn(h,x)->forward[0];
}
uint64_t sl_find_member(ShmHandle *h,ZSetHeader *zsh,const void *m,uint32_t ml)
{
    uint64_t cur=core_sn(h,zsh->head_offset)->forward[0];
    while(cur!=OFFSET_NULL){
        SkipNode *sn=core_sn(h,cur);
        if(sn->member_len==ml&&memcmp(OFF2PTR(h,sn->member_offset),m,ml)==0)return cur;
        cur=sn->forward[0];
    }
    return OFFSET_NULL;
}
uint64_t sl_node_alloc(ShmHandle *h,double sc,const void *m,uint32_t ml,uint32_t lv)
{
    uint64_t n=heap_alloc(h,sizeof(SkipNode)), mo=heap_alloc(h,ml);
    if(n==OFFSET_NULL||mo==OFFSET_NULL){
		if(n!=OFFSET_NULL)heap_free(h,n);
		if(mo!=OFFSET_NULL)heap_free(h,mo);
		return OFFSET_NULL;
	}
    memcpy(OFF2PTR(h,mo),m,ml);
    SkipNode *sn=core_sn(h,n);
    sn->score=sc;sn->member_offset=mo;sn->member_len=ml;sn->level_count=lv;sn->backward_offset=OFFSET_NULL;
    for(int i=0;i<ZSET_MAX_LEVEL;i++)sn->forward[i]=OFFSET_NULL;
    return n;
}
void sl_node_free(ShmHandle *h,uint64_t n){if(n==OFFSET_NULL)return;heap_free(h,core_sn(h,n)->member_offset);heap_free(h,n);}
void sl_unlink(ShmHandle *h,ZSetHeader *zsh,uint64_t n_off,uint64_t upd[ZSET_MAX_LEVEL])
{
    SkipNode *sn=core_sn(h,n_off);
    for(int i=0;i<(int)zsh->cur_level;i++) if(core_sn(h,upd[i])->forward[i]==n_off) core_sn(h,upd[i])->forward[i]=sn->forward[i];
    if(sn->forward[0]!=OFFSET_NULL) core_sn(h,sn->forward[0])->backward_offset=upd[0];
    else zsh->tail_offset=upd[0];
    while(zsh->cur_level>1&&core_sn(h,zsh->head_offset)->forward[zsh->cur_level-1]==OFFSET_NULL) zsh->cur_level--;
}
