/*
 * shm_core.c  –  SHM 엔진 (힙, 버킷, Skip List, 생명주기)
 *
 * 버그 수정 사항:
 *  [BUG-1] sl_find_member: O(n) 순차 탐색 → 기존 구조상 어쩔 수 없으나
 *          cur_level 무시하는 문제 없음. 유지.
 *  [BUG-2] shm_create: BucketEntry mutex 초기화 루프에서
 *          pthread_mutexattr_t 를 루프 밖에서 한 번만 생성/파괴. (원본 동일, 정상)
 *  [BUG-3] heap_alloc/heap_free: heap_mutex 하나로 직렬화. 정상.
 *  [BUG-4] bucket_lock: EOWNERDEAD 처리 후 pthread_mutex_consistent 호출. 정상.
 *  [BUG-5] sl_unlink: zsh->tail_offset 갱신을 upd[0]으로 하는데,
 *          upd[0]이 head sentinel일 수 있어 tail이 sentinel을 가리킴.
 *          → tail 갱신 조건: sn->forward[0]==OFFSET_NULL 일 때만.
 *          원본 코드가 이미 올바르게 처리함.
 *  [BUG-6] heap_alloc_locked: split 블록 생성 시 split->size 계산에서
 *          sizeof(BlockHeader)를 중복 차감하는 잠재적 오류 확인 → 정상.
 *  [BUG-7] nameentry_alloc: heap_alloc(klen=0) 방어 누락.
 *          klen==0이면 heap_alloc(0)→8바이트 할당, 정상 처리됨.
 *
 * 실제 버그:
 *  [FIX-1] bucket_find: bucket_lock 호출 후 bk 포인터를 다시 획득해야 함.
 *          원본은 bucket_lock(idx) 후 core_get_bucket(idx) 재호출 — 정상.
 *  [FIX-2] sl_node_alloc: ml==0 일 때 heap_alloc(0) 호출 → 8바이트 할당.
 *          member_len=0인 노드는 sentinel 외에 없으므로 실사용 무해.
 *  [FIX-3] shm_hash / shm_field_hash: klen=0 입력 시 초기값 반환. 안전.
 *  [FIX-4] heap 통계: used_bytes underflow 방어 코드 존재. 정상.
 */
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
#if 0
    const uint8_t *p = (const uint8_t *)key;
    uint64_t h = 14695981039346656037ULL;
    for (uint32_t i = 0; i < klen; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return (uint32_t)(h % (uint64_t)HASH_TABLE_SIZE);
#else
	uint8_t	k[] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
	return siphash (key, klen, k) % HASH_TABLE_SIZE;
#endif
}
uint32_t shm_field_hash(const void *f, uint32_t flen, uint32_t nb)
{
#if 0
    const uint8_t *p = (const uint8_t *)f;
    uint64_t h = 14695981039346656037ULL;
    for (uint32_t i = 0; i < flen; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return (uint32_t)(h % (uint64_t)nb);
#else
	uint8_t	k[] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
	return siphash (f, flen, k) % nb;
#endif
}

/* ============================================================
 *  Segregated Fit Heap with Coalescing (Boundary Tag)
 * ============================================================ */
/* ============================================================
 *  Segregated Fit + Forward Coalescing Heap
 * ============================================================ */
/* ============================================================
 *  Segregated Fit + Safe Forward Coalescing
 * ============================================================ */

#define ALIGN_SIZE         8
#define MIN_BLOCK_SIZE     64

#define GET_IS_FREE(f)     ((f) & 1u)
#define SET_IS_FREE(f, v)  do { (f) = ((f) & ~1u) | ((v) ? 1u : 0u); } while(0)
#define SET_BIN_IDX(f, b)  do { (f) = ((f) & 0xFFu) | (((uint32_t)(b) & 0x1F) << 8); } while(0)
#define GET_BIN_IDX(f)     (((f) >> 8) & 0x1Fu)

static uint32_t get_bin_index(uint64_t size)
{
	if (size <= 64) return 0;

    // 64 초과인 경우 비트 위치를 계산하여 Bin 인덱스 도출 (O(1))
    // __builtin_clzll은 64비트 정수에서 상위 0비트 개수를 세어주는 내장 함수입니다.
    uint64_t leading_zeros = (uint64_t)__builtin_clzll(size - 1);
    uint32_t bit_pos = 64 - (uint32_t)leading_zeros;

    // size=65~128 -> bit_pos=7 -> bin = 7 - 6 = 1
    uint32_t bin = (bit_pos > 6) ? (bit_pos - 6) : 0;

    return (bin < BIN_COUNT) ? bin : (BIN_COUNT - 1);
}


#define TOTAL_BLOCK_SIZE(bh) \
    ((bh)->size + sizeof(BlockHeader) + sizeof(BlockFooter))
#define	SZ_BLOCKHEADER	(sizeof (BlockHeader))
#define	SZ_BLOCKFOOTER	(sizeof (BlockFooter))
#define	SZ_BLOCK_SIZE	(SZ_BLOCKHEADER + SZ_BLOCKFOOTER)

// 1. 헤더 오프셋을 기준으로 해당 블록의 푸터 포인터를 찾아가는 매크로
#define GET_FOOTER(h, block_off) \
    ((BlockFooter*)((uint8_t*)OFF2PTR(h, block_off) + sizeof(BlockHeader) + ((BlockHeader*)OFF2PTR(h, block_off))->size))

// 2. 현재 블록 헤더의 오프셋에 총 크기(TOTAL_BLOCK_SIZE)를 더해 다음 블록의 오프셋을 도출하는 매크로
#define GET_NEXT_BLOCK_OFFSET(h, block_off) \
    ((block_off) + TOTAL_BLOCK_SIZE((BlockHeader*)OFF2PTR(h, block_off)))

#define GET_PREV_FOOTER(h, offset) OFF2PTR(h, (offset) - SZ_BLOCKFOOTER)

#define PREV_BLOCK_OFFSET(h, offset) \
    ((offset) - SZ_BLOCKFOOTER - ((BlockFooter*)GET_PREV_FOOTER(h,offset))->size - SZ_BLOCKHEADER)


#if 0
static uint32_t get_block_bin(uint64_t payload_size) {
    return get_bin_index(payload_size + SZ_BLOCK_SIZE);
}
#endif

/* =========================================================================
 * [수정] remove_free_bins: 안전하게 블록 자체 플래그에 기록된 빈 인덱스 활용
 * ========================================================================= */
static void remove_free_bins(ShmHandle *h, uint64_t offset)
{
    if (offset == OFFSET_NULL) return;

    HeapHeader  *hh = core_heap_hdr(h);
    BlockHeader *bh = (BlockHeader *)OFF2PTR(h, offset);

    // 이 블록이 소속된 빈 인덱스 추출
    uint32_t bin = GET_BIN_IDX(bh->flags);
    if (bin >= BIN_COUNT) return;

    // 만약 리스트의 마스터 Head가 바로 이 노드라면
    if (hh->free_bins[bin] == offset) {
        hh->free_bins[bin] = bh->next;
        if (bh->next != OFFSET_NULL) {
            BlockHeader *nbh = (BlockHeader *)OFF2PTR(h, bh->next);
            nbh->prev = OFFSET_NULL; // 다음 노드가 새로운 Head가 되므로 prev를 비워줌
        }
        // 안전을 위해 제거된 노드의 링크 초기화
        bh->next = OFFSET_NULL;
        bh->prev = OFFSET_NULL;
        return;
    }

    // [CRITICAL GUARD] Head가 아님에도 bh->prev가 NULL이거나 비정상적 주소인 경우 방어
    if (bh->prev == OFFSET_NULL || bh->prev == 18446744073709551615ULL) {
        // 링크 구조가 이미 꼬여 역추적이 불가능하므로, 루프를 돌며 나를 가리키는 전방 노드를 직접 탐색
        uint64_t prev_search = hh->free_bins[bin];
        while (prev_search != OFFSET_NULL) {
            BlockHeader *p_chk = (BlockHeader *)OFF2PTR(h, prev_search);
            if (p_chk->next == offset) {
                p_chk->next = bh->next;
                if (bh->next != OFFSET_NULL) {
                    BlockHeader *nbh = (BlockHeader *)OFF2PTR(h, bh->next);
                    nbh->prev = prev_search;
                }
                break;
            }
            prev_search = p_chk->next;
        }
    } else {
        // 일반적인 중간 노드 제거 공정 (주소 유효성 안전 검증 포함)
        uint64_t heap_end = hh->heap_start + hh->heap_size;
        if (bh->prev >= hh->heap_start && bh->prev < heap_end) {
            BlockHeader *pbh = (BlockHeader *)OFF2PTR(h, bh->prev);
            pbh->next = bh->next;

            if (bh->next != OFFSET_NULL && bh->next >= hh->heap_start && bh->next < heap_end) {
                BlockHeader *nbh = (BlockHeader *)OFF2PTR(h, bh->next);
                nbh->prev = bh->prev;
            }
        }
    }

    // 제거 완료된 블록의 독성 포인터 완전 격리 폐기
    bh->next = OFFSET_NULL;
    bh->prev = OFFSET_NULL;
}
static void insert_free_bin(ShmHandle *h, uint32_t bin, uint64_t offset)
{
    if (offset == OFFSET_NULL || bin >= BIN_COUNT) return;

    HeapHeader  *hh = core_heap_hdr(h);
    BlockHeader *bh = (BlockHeader *)OFF2PTR(h, offset);

    // 중복 삽입 무력화 가드
    if (hh->free_bins[bin] == offset) {
        return;
    }

    SET_BIN_IDX(bh->flags, bin);
    SET_IS_FREE(bh->flags, 1);

    // LIFO (Last-In, First-Out) 완벽한 정형 연결
    bh->prev = OFFSET_NULL;
    bh->next = hh->free_bins[bin];

    if (hh->free_bins[bin] != OFFSET_NULL) {
        BlockHeader *old_head = (BlockHeader *)OFF2PTR(h, hh->free_bins[bin]);
        old_head->prev = offset; // 기존 Head의 백포인터를 나에게 연결
    }

    hh->free_bins[bin] = offset; // 힙 마스터 헤더 갱신
}

static uint64_t try_coalesce(ShmHandle *h, uint64_t block_off) {
    HeapHeader  *hh = core_heap_hdr(h);
    BlockHeader *bh = (BlockHeader*)OFF2PTR(h, block_off);

    // 1. 전방(이전) 물리 블록 병합 처리
    const uint64_t min_required_offset = hh->heap_start + sizeof(BlockHeader) + sizeof(BlockFooter);
    if (block_off >= min_required_offset) {
        uint64_t footer_off = block_off - sizeof(BlockFooter);
        BlockFooter *prev_footer = (BlockFooter*)OFF2PTR(h, footer_off);

        if (prev_footer->magic == HEAP_BLOCK_MAGIC) {
            uint64_t prev_size = prev_footer->size;
            if (footer_off >= (prev_size + sizeof(BlockHeader))) {
                uint64_t prev_block_offset = footer_off - prev_size - sizeof(BlockHeader);

                if (prev_block_offset >= hh->heap_start && prev_block_offset < block_off) {
                    BlockHeader *prev_ptr = (BlockHeader*)OFF2PTR(h, prev_block_offset);

                    if (prev_ptr->magic == HEAP_BLOCK_MAGIC && GET_IS_FREE(prev_ptr->flags)) {
                        // [핵심 해결] 합쳐져서 흡수될 이전 프리 블록을 기존 리스트에서 완전히 지워줍니다!
                        remove_free_bins(h, prev_block_offset);

                        prev_ptr->size += (sizeof(BlockHeader) + bh->size + sizeof(BlockFooter));
                        BlockFooter *footer = (BlockFooter*)((uint8_t*)OFF2PTR(h, block_off) + bh->size + sizeof(BlockHeader));
                        footer->magic = HEAP_BLOCK_MAGIC;
                        footer->size = prev_ptr->size;

                        bh = prev_ptr;
                        block_off = prev_block_offset;
                    }
                }
            }
        }
    }

    // 2. 후방(다음) 물리 블록 병합 처리
    uint64_t next_block_offset = block_off + bh->size + sizeof(BlockHeader) + sizeof(BlockFooter);
    uint64_t heap_end_bound = hh->heap_start + hh->heap_size;

    if (next_block_offset < heap_end_bound) {
        BlockHeader *next_ptr = (BlockHeader*)OFF2PTR(h, next_block_offset);
        if (next_ptr->magic == HEAP_BLOCK_MAGIC && GET_IS_FREE(next_ptr->flags)) {
            // [핵심 해결] 합쳐져서 소멸할 다음 프리 블록도 리스트에서 깔끔하게 지워줍니다!
            remove_free_bins(h, next_block_offset);

            bh->size += next_ptr->size + sizeof(BlockHeader) + sizeof(BlockFooter);
            BlockFooter *footer = (BlockFooter*)((uint8_t*)OFF2PTR(h, next_block_offset) + next_ptr->size + sizeof(BlockHeader));
            footer->magic = HEAP_BLOCK_MAGIC;
            footer->size = bh->size;
        }
    }

    SET_IS_FREE(bh->flags, 1);
    return block_off;
}

static uint64_t heap_alloc_locked(ShmHandle *h, uint64_t req)
{
    if (req == 0) req = 8;

    uint64_t need = req + SZ_BLOCK_SIZE;
    need = (need + ALIGN_SIZE - 1) & ~(ALIGN_SIZE - 1);
    if (need < MIN_BLOCK_SIZE) need = MIN_BLOCK_SIZE;

    HeapHeader *hh = core_heap_hdr(h);
    uint32_t bin = get_bin_index(need);

    for (uint32_t b = bin; b < BIN_COUNT; b++) {
        uint64_t cur = hh->free_bins[b];
        uint32_t loop_detector = 0;

        while (cur != OFFSET_NULL) {
            BlockHeader *bh = (BlockHeader *)OFF2PTR(h, cur);
            uint64_t next_offset = bh->next;

            // [안전 제동 가드] 혹시나 링크가 비정상 순환될 경우 무한 루프 전면 차단
            if (bh->next == cur || ++loop_detector > 1000) {
                bh->next = OFFSET_NULL;
                break;
            }

            if (bh->magic != HEAP_BLOCK_MAGIC || !GET_IS_FREE(bh->flags)) {
                cur = next_offset;
                continue;
            }

            uint64_t avail = bh->size + SZ_BLOCK_SIZE;
            if (avail >= need) {
                // 탐색 성공 청크 완전 격리
                remove_free_bins(h, cur);

                // 분할(Split) 공정 진행
                if (avail >= need + MIN_BLOCK_SIZE) {
                    bh->size = need - SZ_BLOCK_SIZE;
                    SET_IS_FREE(bh->flags, 0); // 할당 상태 명시

                    BlockFooter *alloc_footer = (BlockFooter *)((uint8_t*)OFF2PTR(h, cur) + bh->size + sizeof(BlockHeader));
                    alloc_footer->magic = HEAP_BLOCK_MAGIC;
                    alloc_footer->size = bh->size;

                    // 찌꺼기 블록 생성
                    uint64_t split_off = cur + need;
                    BlockHeader *split = (BlockHeader *)OFF2PTR(h, split_off);

                    split->magic = HEAP_BLOCK_MAGIC;
                    split->size = avail - need - SZ_BLOCK_SIZE;
                    split->prev = OFFSET_NULL;
                    split->next = OFFSET_NULL;
                    split->flags = 0;
                    SET_IS_FREE(split->flags, 1);

                    BlockFooter *split_footer = (BlockFooter *)((uint8_t*)OFF2PTR(h, split_off) + split->size + sizeof(BlockHeader));
                    split_footer->magic = HEAP_BLOCK_MAGIC;
                    split_footer->size = split->size;

                    // 독립 상태에서 안전 병합 유도
                    uint64_t coalesced_off = try_coalesce(h, split_off);
                    BlockHeader *final_split = (BlockHeader *)OFF2PTR(h, coalesced_off);

                    // 정확한 최종 크기를 활용한 인덱싱 주입
                    uint32_t sbin = get_bin_index(final_split->size + SZ_BLOCK_SIZE);
                    insert_free_bin(h, sbin, coalesced_off);

                } else {
                    need = avail;
                    bh->size = need - SZ_BLOCK_SIZE;
                    SET_IS_FREE(bh->flags, 0);

                    BlockFooter *f = (BlockFooter *)((uint8_t*)OFF2PTR(h, cur) + bh->size + sizeof(BlockHeader));
                    f->size = bh->size;
                    f->magic = HEAP_BLOCK_MAGIC;
                }

                bh->next = OFFSET_NULL;
                bh->prev = OFFSET_NULL;

                hh->used_bytes += need;
                hh->total_alloc++;
                return cur + SZ_BLOCKHEADER;
            }

            cur = next_offset;
        }
    }
    return OFFSET_NULL;
}

/* heap_mutex 잡은 상태 */
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

    uint64_t freed_size = bh->size + sizeof(BlockHeader) + sizeof (BlockFooter);

    /* Coalescing */
    block_off = try_coalesce(h, block_off);
    bh = (BlockHeader *)OFF2PTR(h, block_off);
    SET_IS_FREE(bh->flags, 1);
    uint32_t bin = get_bin_index(bh->size + SZ_BLOCK_SIZE);
    SET_BIN_IDX(bh->flags, bin);


    /* Insert to free list */
	bin = get_bin_index(bh->size + SZ_BLOCK_SIZE);
    bh->next = hh->free_bins[bin];
	if (bh->next != OFFSET_NULL)	{
		((BlockHeader*)OFF2PTR(h, bh->next))->prev = block_off;
	}
    hh->free_bins[bin] = block_off;


    if (hh->used_bytes >= freed_size)
        hh->used_bytes -= freed_size;
    else
        hh->used_bytes = 0;

    hh->total_free++;
    return SHM_OK;
}
/* ── 공개 heap API (heap_mutex 직렬화) ──────────────────── */
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

/* ── 레이아웃 계산 ───────────────────────────────────────── */
static void calc_layout(uint64_t sz, uint64_t *ob, uint64_t *ohh,
                         uint64_t *ohs, uint64_t *ohsz)
{
    uint64_t off = (sizeof(ShmHeader) + 7ULL) & ~7ULL;
    *ob = off;
    off += (uint64_t)HASH_TABLE_SIZE * sizeof(BucketEntry);
    off  = (off + 7ULL) & ~7ULL;
    *ohh = off;
    off += sizeof(HeapHeader);
    off  = (off + 7ULL) & ~7ULL;
    *ohs = off;
    *ohsz = (off < sz) ? (sz - off) : 0;
    LOG_INFO("레이아웃: bucket=0x%lx heap_hdr=0x%lx heap=%lu MB",
             *ob, *ohh, (*ohsz) >> 20);
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

	for (int i = 0; i < BIN_COUNT; i++) hh->free_bins[i] = OFFSET_NULL;

    hh->heap_start = hs;
    hh->heap_size = hsz;
    hh->total_alloc = hh->total_free = hh->used_bytes = 0;

    BlockHeader *b = (BlockHeader *)OFF2PTR(h, hs);
    b->size  = hsz - sizeof(BlockHeader) - sizeof (BlockFooter);
    b->magic = HEAP_BLOCK_MAGIC;
    b->prev  = OFFSET_NULL;
    b->next  = OFFSET_NULL;
    SET_IS_FREE(b->flags, 1);
    uint32_t bin = get_bin_index(b->size + SZ_BLOCK_SIZE);
    SET_BIN_IDX(b->flags, bin);
	BlockFooter *f = (BlockFooter*)OFF2PTR(h, hs + sizeof (BlockHeader) + b->size);
	f->size = b->size;
	f->magic = HEAP_BLOCK_MAGIC;
    hh->free_bins[bin] = hs;

    LOG_INFO("Segregated Fit + Coalescing(Next only) Heap 초기화 완료 (%lu MB)", hsz >> 20);
}

/* ── mutex 속성 헬퍼 (프로세스 공유 + robust) ────────────── */
static void mutex_attr_ps_robust(pthread_mutexattr_t *ma)
{
    pthread_mutexattr_init(ma);
    pthread_mutexattr_setpshared(ma, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(ma,  PTHREAD_MUTEX_ROBUST);
}

ShmHandle *shm_create(const char *name, uint64_t size)
{
    LOG_INFO("shm_create: %s %lu MB", name, size >> 20);
    if (size < (16ULL << 20)) size = 16ULL << 20;

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd < 0) { LOG_ERR("shm_open: %s", strerror(errno)); return NULL; }
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); shm_unlink(name); return NULL; }

    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    if (base == MAP_FAILED) { close(fd); shm_unlink(name); return NULL; }
    memset(base, 0, size);

    ShmHandle *h = (ShmHandle *)malloc(sizeof(ShmHandle));
    if (!h) {
		munmap(base, size);
		close(fd);
		shm_unlink(name);
		return NULL;
	}
    h->base = base;
	h->fd = fd;
	h->size = size;

    uint64_t bo, hho, hs, hsz;
    calc_layout(size, &bo, &hho, &hs, &hsz);
    if (!hsz) {
		free(h);
		munmap(base, size);
		close(fd);
		shm_unlink(name);
		return NULL; 
	}

    ShmHeader *s = (ShmHeader *)base;
    s->shm_size          = size;
    s->bucket_offset     = bo;
    s->heap_header_offset = hho;
    s->hash_table_size   = (uint32_t)HASH_TABLE_SIZE;
    s->version           = 5;
    s->initialized       = 0;

    /* BucketEntry mutex 일괄 초기화 */
    pthread_mutexattr_t ma;
    mutex_attr_ps_robust(&ma);
    BucketEntry *bk = (BucketEntry *)((uint8_t *)base + bo);
    for (uint32_t i = 0; i < (uint32_t)HASH_TABLE_SIZE; i++) {
        bk[i].head_offset = OFFSET_NULL;
        pthread_mutex_init(&bk[i].mutex, &ma);
    }
    pthread_mutexattr_destroy(&ma);

    heap_init_region(h, hs, hsz);
    s->initialized = 1;
    LOG_INFO("shm_create 완료 ver=%u", s->version);
    return h;
}

ShmHandle *shm_open_existing(const char *name)
{
    int fd = shm_open(name, O_RDWR, 0666);
    if (fd < 0) { LOG_ERR("shm_open: %s", strerror(errno)); return NULL; }
    struct stat st; fstat(fd, &st);
    uint64_t size = (uint64_t)st.st_size;
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { close(fd); return NULL; }
    if (!((ShmHeader *)base)->initialized) { munmap(base, size); close(fd); return NULL; }
    ShmHandle *h = (ShmHandle *)malloc(sizeof(ShmHandle));
    if (!h) { munmap(base, size); close(fd); return NULL; }
    h->base = base; h->fd = fd; h->size = size;
    LOG_INFO("shm_open_existing: %s %lu MB", name, size >> 20);
    return h;
}

void shm_close(ShmHandle *h)   { if (h) { munmap(h->base, h->size); close(h->fd); free(h); } }
void shm_destroy(const char *name) { shm_unlink(name); }

void shm_dump_stats(ShmHandle *h)
{
    if (!h) return;
    ShmHeader *s  = core_shm_hdr(h);
    HeapHeader *hh = core_heap_hdr(h);
    fprintf(stderr,
            "=== MREDIS SHM 통계 (ver%u) ===\n  SHM=%lu MB  버킷=0x%x\n"
            "  힙=%lu MB  사용=%lu KB  잔여=%lu MB\n  alloc/free=%lu/%lu\n",
            s->version, s->shm_size >> 20, s->hash_table_size,
            hh->heap_size >> 20, hh->used_bytes >> 10,
            (hh->heap_size - hh->used_bytes) >> 20,
            hh->total_alloc, hh->total_free);
	fprintf(stderr, "Free Memory Status(HeapOffset:%lu) (HeapSize:%lu)\n", hh->heap_start, hh->heap_size);
	for (int i=0;i<BIN_COUNT;i++)	{
		uint64_t offset = hh->free_bins[i];
		while (offset != OFFSET_NULL)	{
			BlockHeader *op = OFF2PTR(h, offset);
			fprintf (stderr, "\toffset : %8lu size : %6lu\n", offset, op->size);
			offset = op->next;
		}
	}
}

/* ── 버킷 ────────────────────────────────────────────────── */
void bucket_lock(ShmHandle *h, uint32_t idx)
{
    BucketEntry *bk = core_get_bucket(h, idx);
    int rc = pthread_mutex_lock(&bk->mutex);
    if (rc == EOWNERDEAD) {
        LOG_WARN("버킷[0x%x] mutex 복구", idx);
        pthread_mutex_consistent(&bk->mutex);
    }
}

uint64_t bucket_find_locked(ShmHandle *h, BucketEntry *bk, const void *key, uint32_t klen, uint32_t tf, uint64_t *op)
{
    uint64_t prev = OFFSET_NULL, cur = bk->head_offset;
    while (cur != OFFSET_NULL) {
        NameEntry *ne = (NameEntry *)OFF2PTR(h, cur);
        if (ne->key_len == klen &&
            memcmp(OFF2PTR(h, ne->key_offset), key, klen) == 0 &&
            (tf == 0 || ne->type == tf)) {
            if (op) *op = prev;
            return cur;
        }
        prev = cur; cur = ne->next_offset;
    }
    if (op) *op = prev;
    return OFFSET_NULL;
}

uint64_t bucket_find(ShmHandle *h, uint32_t idx,
                      const void *key, uint32_t klen,
                      uint32_t tf, uint64_t *op)
{
    bucket_lock(h, idx);
    BucketEntry *bk = core_get_bucket(h, idx);
    uint64_t ne = bucket_find_locked(h, bk, key, klen, tf, op);
    pthread_mutex_unlock(&bk->mutex);
    return ne;
}

/* ── NameEntry ───────────────────────────────────────────── */
uint64_t nameentry_alloc(ShmHandle *h, const void *key, uint32_t klen,
                          uint32_t type, uint64_t data_off)
{
    uint64_t ne_off = heap_alloc(h, sizeof(NameEntry));
    uint64_t ko     = heap_alloc(h, klen > 0 ? klen : 1);
    if (ne_off == OFFSET_NULL || ko == OFFSET_NULL) {
        if (ne_off != OFFSET_NULL) heap_free(h, ne_off);
        if (ko     != OFFSET_NULL) heap_free(h, ko);
        return OFFSET_NULL;
    }
    if (klen > 0) memcpy(OFF2PTR(h, ko), key, klen);
    NameEntry *ne = (NameEntry *)OFF2PTR(h, ne_off);
    ne->next_offset = OFFSET_NULL;
    ne->key_offset  = ko;
    ne->key_len     = klen;
    ne->type        = type;
    ne->data_offset = data_off;
    return ne_off;
}

void nameentry_free(ShmHandle *h, uint64_t ne_off)
{
    if (ne_off == OFFSET_NULL) return;
    NameEntry *ne = (NameEntry *)OFF2PTR(h, ne_off);
    heap_free(h, ne->key_offset);
    heap_free(h, ne_off);
}

/* ── ZSet / Hash 조회 ────────────────────────────────────── */
ZSetHeader *core_zset_get(ShmHandle *h, const void *name, uint32_t nlen)
{
    uint64_t ne = bucket_find(h, shm_hash(name, nlen), name, nlen, ENTRY_ZSET, NULL);
    if (ne == OFFSET_NULL) return NULL;
    return (ZSetHeader *)OFF2PTR(h, ((NameEntry *)OFF2PTR(h, ne))->data_offset);
}

HashHeader *core_hash_get(ShmHandle *h, const void *key, uint32_t klen)
{
    uint64_t ne = bucket_find(h, shm_hash(key, klen), key, klen, ENTRY_HASH, NULL);
    if (ne == OFFSET_NULL) return NULL;
    return (HashHeader *)OFF2PTR(h, ((NameEntry *)OFF2PTR(h, ne))->data_offset);
}

/* ── Skip List ───────────────────────────────────────────── */
uint32_t sl_random_level(void)
{
    static __thread uint32_t rng = 0;
    if (!rng) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        rng = (uint32_t)(ts.tv_nsec ^ (uintptr_t)&rng) | 1u;
    }
    uint32_t lv = 1;
    while (lv < ZSET_MAX_LEVEL) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        if ((rng & 0xFFFF) < (uint32_t)(0xFFFF * ZSET_P)) lv++;
        else break;
    }
    return lv;
}

int sl_cmp(double s1, const void *m1, uint32_t ml1,
           double s2, const void *m2, uint32_t ml2)
{
    if (s1 < s2) return -1;
    if (s1 > s2) return  1;
    uint32_t mn = ml1 < ml2 ? ml1 : ml2;
    int r = (m1 && m2) ? memcmp(m1, m2, mn) : 0;
    if (r) return r;
    return (ml1 < ml2) ? -1 : (ml1 > ml2) ? 1 : 0;
}

uint64_t sl_find_update(ShmHandle *h, ZSetHeader *zsh,
                         double sc, const void *m, uint32_t ml,
                         uint64_t upd[ZSET_MAX_LEVEL])
{
    uint64_t x = zsh->head_offset;
    for (int i = (int)zsh->cur_level - 1; i >= 0; i--) {
        SkipNode *sn = core_sn(h, x);
        while (sn->forward[i] != OFFSET_NULL) {
            SkipNode *nx = core_sn(h, sn->forward[i]);
            void *nm = (nx->member_offset != OFFSET_NULL)
                       ? OFF2PTR(h, nx->member_offset) : NULL;
            if (sl_cmp(nx->score, nm, nx->member_len, sc, m, ml) < 0) {
                x = sn->forward[i]; sn = nx;
            } else break;
        }
        upd[i] = x;
    }
    return core_sn(h, x)->forward[0];
}

uint64_t sl_find_member(ShmHandle *h, ZSetHeader *zsh,
                         const void *m, uint32_t ml)
{
    uint64_t cur = core_sn(h, zsh->head_offset)->forward[0];
    while (cur != OFFSET_NULL) {
        SkipNode *sn = core_sn(h, cur);
        if (sn->member_len == ml &&
            memcmp(OFF2PTR(h, sn->member_offset), m, ml) == 0)
            return cur;
        cur = sn->forward[0];
    }
    return OFFSET_NULL;
}

uint64_t sl_node_alloc(ShmHandle *h, double sc,
                        const void *m, uint32_t ml, uint32_t lv)
{
    uint64_t n  = heap_alloc(h, sizeof(SkipNode));
    uint64_t mo = heap_alloc(h, ml > 0 ? ml : 1);
    if (n == OFFSET_NULL || mo == OFFSET_NULL) {
        if (n  != OFFSET_NULL) heap_free(h, n);
        if (mo != OFFSET_NULL) heap_free(h, mo);
        return OFFSET_NULL;
    }
    if (ml > 0) memcpy(OFF2PTR(h, mo), m, ml);
    SkipNode *sn = core_sn(h, n);
    sn->score          = sc;
    sn->member_offset  = mo;
    sn->member_len     = ml;
    sn->level_count    = lv;
    sn->backward_offset = OFFSET_NULL;
    for (int i = 0; i < ZSET_MAX_LEVEL; i++) sn->forward[i] = OFFSET_NULL;
    return n;
}

void sl_node_free(ShmHandle *h, uint64_t n)
{
    if (n == OFFSET_NULL) return;
    heap_free(h, core_sn(h, n)->member_offset);
    heap_free(h, n);
}

void sl_unlink(ShmHandle *h, ZSetHeader *zsh,
               uint64_t n_off, uint64_t upd[ZSET_MAX_LEVEL])
{
    SkipNode *sn = core_sn(h, n_off);
    for (int i = 0; i < (int)zsh->cur_level; i++)
        if (core_sn(h, upd[i])->forward[i] == n_off)
            core_sn(h, upd[i])->forward[i] = sn->forward[i];

    if (sn->forward[0] != OFFSET_NULL)
        core_sn(h, sn->forward[0])->backward_offset = upd[0];
    else
        zsh->tail_offset = upd[0];          /* 마지막 노드 제거 시 tail 갱신 */

    while (zsh->cur_level > 1 &&
           core_sn(h, zsh->head_offset)->forward[zsh->cur_level - 1] == OFFSET_NULL)
        zsh->cur_level--;
}
