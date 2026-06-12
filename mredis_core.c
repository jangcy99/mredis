/*
 * mredis_core.c  –  SHM 엔진 (힙, 버킷, 구조적 안정성 커널 최적화 버전)
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
#include "mredis_types.h"
#include "mredis_core.h"

int g_mredis_dbg = SHM_DEBUG_LEVEL;
void mredis_set_debug_level(int l) { g_mredis_dbg = l; }

const char *mredis_strerror(int e)
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
uint64_t mredis_hash(const void *key, uint32_t klen)
{
    if (klen == 0 || !key) return 0; // 명시적 방어 코드 추가
    const uint8_t *p = (const uint8_t *)key;
    uint64_t h = 14695981039346656037ULL;
    for (uint32_t i = 0; i < klen; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}
uint32_t mredis_field_hash(const void *f, uint32_t flen, uint32_t nb)
{
    if (flen == 0 || !f || nb == 0) return 0;
    const uint8_t *p = (const uint8_t *)f;
    uint64_t h = 14695981039346656037ULL;
    for (uint32_t i = 0; i < flen; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return (uint32_t)(h % (uint64_t)nb);
}

/* ============================================================
 * Segregated Fit Heap Layout Macros
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
    uint64_t leading_zeros = (uint64_t)__builtin_clzll(size - 1);
    uint32_t bit_pos = 64 - (uint32_t)leading_zeros;
    uint32_t bin = (bit_pos > 6) ? (bit_pos - 6) : 0;
    return (bin < BIN_COUNT) ? bin : (BIN_COUNT - 1);
}

#define TOTAL_BLOCK_SIZE(bh) \
    ((bh)->size + sizeof(BlockHeader) + sizeof(BlockFooter))
#define	SZ_BLOCKHEADER	(sizeof (BlockHeader))
#define	SZ_BLOCKFOOTER	(sizeof (BlockFooter))
#define	SZ_BLOCK_SIZE	(SZ_BLOCKHEADER + SZ_BLOCKFOOTER)

#define GET_FOOTER(h, block_off) \
    ((BlockFooter*)((uint8_t*)OFF2PTR(h, block_off) + sizeof(BlockHeader) + ((BlockHeader*)OFF2PTR(h, block_off))->size))

#define GET_NEXT_BLOCK_OFFSET(h, block_off) \
    ((block_off) + TOTAL_BLOCK_SIZE((BlockHeader*)OFF2PTR(h, block_off)))

#define GET_PREV_FOOTER(h, offset) OFF2PTR(h, (offset) - SZ_BLOCKFOOTER)

#define PREV_BLOCK_OFFSET(h, offset) \
    ((offset) - SZ_BLOCKFOOTER - ((BlockFooter*)GET_PREV_FOOTER(h,offset))->size - SZ_BLOCKHEADER)


/* =========================================================================
 * Free List 제어 기본 서브루틴
 * ========================================================================= */
static void remove_free_bins(MRedisHandle *h, uint64_t offset)
{
    if (offset == OFFSET_NULL) return;

    HeapHeader  *hh = core_heap_hdr(h);
    BlockHeader *bh = (BlockHeader *)OFF2PTR(h, offset);

    uint32_t bin = GET_BIN_IDX(bh->flags);
    if (bin >= BIN_COUNT) return;

    if (hh->free_bins[bin] == offset) {
        hh->free_bins[bin] = bh->next;
        if (bh->next != OFFSET_NULL) {
            BlockHeader *nbh = (BlockHeader *)OFF2PTR(h, bh->next);
            nbh->prev = OFFSET_NULL;
        }
        bh->next = OFFSET_NULL;
        bh->prev = OFFSET_NULL;
        return;
    }

    if (bh->prev == OFFSET_NULL)	{
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

    bh->next = OFFSET_NULL;
    bh->prev = OFFSET_NULL;
}

static void insert_free_bin(MRedisHandle *h, uint32_t bin, uint64_t offset)
{
    if (offset == OFFSET_NULL || bin >= BIN_COUNT) return;

    HeapHeader  *hh = core_heap_hdr(h);
    BlockHeader *bh = (BlockHeader *)OFF2PTR(h, offset);

    if (hh->free_bins[bin] == offset) return;

    SET_BIN_IDX(bh->flags, bin);
    SET_IS_FREE(bh->flags, 1);

    bh->prev = OFFSET_NULL;
    bh->next = hh->free_bins[bin];

    if (hh->free_bins[bin] != OFFSET_NULL) {
        BlockHeader *old_head = (BlockHeader *)OFF2PTR(h, hh->free_bins[bin]);
        old_head->prev = offset;
    }

    hh->free_bins[bin] = offset;
}

/* [FIXED-2] try_coalesce: 매크로 기반 주소 연산 전환 및 병합 흐름 무결성 확보 */
static uint64_t try_coalesce(MRedisHandle *h, uint64_t block_off) {
    HeapHeader  *hh = core_heap_hdr(h);
    BlockHeader *bh = (BlockHeader*)OFF2PTR(h, block_off);

    // 1. 전방 물리 블록 병합
    const uint64_t min_required_offset = hh->heap_start + SZ_BLOCK_SIZE;
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
                        remove_free_bins(h, prev_block_offset);

                        prev_ptr->size += (sizeof(BlockHeader) + bh->size + sizeof(BlockFooter));
                        BlockFooter *footer = GET_FOOTER(h, prev_block_offset);
                        footer->magic = HEAP_BLOCK_MAGIC;
                        footer->size = prev_ptr->size;

                        bh = prev_ptr;
                        block_off = prev_block_offset;
                    }
                }
            }
        }
    }

    // 2. 후방 물리 블록 병합 (매크로 전면 적용)
    uint64_t next_block_offset = GET_NEXT_BLOCK_OFFSET(h, block_off);
    uint64_t heap_end_bound = hh->heap_start + hh->heap_size;

    if (next_block_offset < heap_end_bound) {
        BlockHeader *next_ptr = (BlockHeader*)OFF2PTR(h, next_block_offset);
        if (next_ptr->magic == HEAP_BLOCK_MAGIC && GET_IS_FREE(next_ptr->flags)) {
            remove_free_bins(h, next_block_offset);

            bh->size += TOTAL_BLOCK_SIZE(next_ptr);
            BlockFooter *footer = GET_FOOTER(h, block_off);
            footer->magic = HEAP_BLOCK_MAGIC;
            footer->size = bh->size;
        }
    }

    SET_IS_FREE(bh->flags, 1);
    return block_off;
}

static uint64_t heap_alloc_locked(MRedisHandle *h, uint64_t req)
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
                remove_free_bins(h, cur);

                if (avail >= need + MIN_BLOCK_SIZE) {
                    bh->size = need - SZ_BLOCK_SIZE;
                    SET_IS_FREE(bh->flags, 0);

                    BlockFooter *alloc_footer = GET_FOOTER(h, cur);
                    alloc_footer->magic = HEAP_BLOCK_MAGIC;
                    alloc_footer->size = bh->size;

                    uint64_t split_off = cur + need;
                    BlockHeader *split = (BlockHeader *)OFF2PTR(h, split_off);

                    split->magic = HEAP_BLOCK_MAGIC;
                    split->size = avail - need - SZ_BLOCK_SIZE;
                    split->prev = OFFSET_NULL;
                    split->next = OFFSET_NULL;
                    split->flags = 0;
                    SET_IS_FREE(split->flags, 1);

                    BlockFooter *split_footer = GET_FOOTER(h, split_off);
                    split_footer->magic = HEAP_BLOCK_MAGIC;
                    split_footer->size = split->size;

                    uint64_t coalesced_off = try_coalesce(h, split_off);
                    BlockHeader *final_split = (BlockHeader *)OFF2PTR(h, coalesced_off);

                    uint32_t sbin = get_bin_index(final_split->size + SZ_BLOCK_SIZE);
                    insert_free_bin(h, sbin, coalesced_off);

                } else {
                    need = avail;
                    bh->size = need - SZ_BLOCK_SIZE;
                    SET_IS_FREE(bh->flags, 0);

                    BlockFooter *f = GET_FOOTER(h, cur);
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

/* [FIXED-1] heap_free_locked: 파편화된 리스트 수동 조작 제거 및 insert_free_bin 표준 함수 일원화 */
static int heap_free_locked(MRedisHandle *h, uint64_t data_off)
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

    uint64_t freed_size = TOTAL_BLOCK_SIZE(bh);

    /* Coalescing 및 인덱스 안정화 */
    block_off = try_coalesce(h, block_off);
    bh = (BlockHeader *)OFF2PTR(h, block_off);
    
    // 안전한 추상화 함수 인터페이스 사용으로 이중 삽입 버그 근본 차단
    uint32_t bin = get_bin_index(bh->size + SZ_BLOCK_SIZE);
    insert_free_bin(h, bin, block_off);

    if (hh->used_bytes >= freed_size)
        hh->used_bytes -= freed_size;
    else
        hh->used_bytes = 0;

    hh->total_free++;
    return SHM_OK;
}

/**
 * @brief Robust Mutex 복구 및 힙 상태 사후 감사(Audit)
 * @param h MRedis 엔진 핸들
 * @param mutex_rc pthread_mutex_lock의 리턴 값
 * @return int 복구 및 검증 성공 시 SHM_OK, 실패 시 SHM_ERR_CORRUPT
 */
int mredis_recover_and_audit(MRedisHandle *h, int mutex_rc)
{
    // 1. 정상적으로 락을 획득한 경우 검증 패스
    if (mutex_rc == 0) return SHM_OK;

    // 2. 이전 소유 프로세스가 죽은 경우 (Robust Mutex 핵심 처리)
    if (mutex_rc == EOWNERDEAD) {
        HeapHeader *hh = core_heap_hdr(h);
        LOG_WARN("MREDIS [CRITICAL]: 이전 소유 프로세스 사망 감지 (EOWNERDEAD). 힙 복구 및 Audit 시작.");

        // Mutex를 다시 일관성 있는(Consistent) 상태로 마킹
        if (pthread_mutex_consistent(&hh->heap_mutex) != 0) {
            LOG_ERR("MREDIS [FATAL]: pthread_mutex_consistent 호출 실패.");
            return SHM_ERR_CORRUPT;
        }

        // 3. 힙 메타데이터 정밀 감사 (Post-Crash Audit)
        uint64_t heap_end = hh->heap_start + hh->heap_size;

        // 가드 조건 1: 힙 경계 주소가 공유 메모리 전체 크기를 벗어나는지 확인
        if (heap_end > h->size) {
            LOG_ERR("MREDIS [AUDIT FAIL]: 힙 영역 경계가 SHM 크기를 초과함. (HeapEnd: %lu > SHM_Size: %lu)", heap_end, h->size);
            return SHM_ERR_CORRUPT;
        }

        // 가드 조건 2: 사용 바이트 수가 전체 힙 크기보다 큰지 역전 현상 체크
        if (hh->used_bytes > hh->heap_size) {
            LOG_ERR("MREDIS [AUDIT FAIL]: 힙 사용량 오버플로우 감지. (Used: %lu > Total: %lu)", hh->used_bytes, hh->heap_size);
            return SHM_ERR_CORRUPT;
        }

        // 가드 조건 3: 최상위 프리 리스트(Free Bins) 오프셋 주소 유효성 체크
        for (int i = 0; i < BIN_COUNT; i++) {
            uint64_t bin_off = hh->free_bins[i];
            if (bin_off != OFFSET_NULL) {
                if (bin_off < hh->heap_start || bin_off >= heap_end) {
                    LOG_ERR("MREDIS [AUDIT FAIL]: Free Bin [%d]의 시작 오프셋이 무효함. (Offset: %lu)", i, bin_off);
                    return SHM_ERR_CORRUPT;
                }
            }
        }

        LOG_INFO("MREDIS [AUDIT PASS]: Robust Mutex 복구 및 1차 메타데이터 검증 완료.");
        return SHM_OK;
    }

    // 기타 정의되지 않은 Mutex 에러 처리
    LOG_ERR("MREDIS [ERROR]: Mutex 락 획득 중 알 수 없는 오류 발생 (rc=%d)", mutex_rc);
    return SHM_ERR;
}

/**
 * @brief 힙 전체 메모리 블록 유효성 전수 검증 (Heap Walker)
 * @param h MRedis 엔진 핸들
 * @return int 무결성 통과 시 SHM_OK, 파손 감지 시 SHM_ERR_CORRUPT
 */
int mredis_heap_validate_integrity(MRedisHandle *h)
{
    HeapHeader *hh = core_heap_hdr(h);
    uint64_t current_off = hh->heap_start;
    uint64_t heap_end = hh->heap_start + hh->heap_size;
    uint64_t calculated_used_bytes = 0;
    uint64_t block_count = 0;

    LOG_INFO("MREDIS [INTEGRITY]: 힙 메모리 블록 전수 무결성 검사 시작...");

    while (current_off < heap_end) {
        block_count++;

        // 1. 블록 헤더 포인터 유효성 검사
        BlockHeader *bh = (BlockHeader *)OFF2PTR(h, current_off);

        // 매직 넘버 검증 (Header)
        if (bh->magic != HEAP_BLOCK_MAGIC) {
            LOG_ERR("MREDIS [CORRUPTION DETECTED]: 블록 [%lu] 헤더 매직 넘버 파손! (Offset: %lu, Magic: 0x%X)",
                    block_count, current_off, bh->magic);
            return SHM_ERR_CORRUPT;
        }

        // 블록 사이즈 상한선 가드 체크
        if (current_off + sizeof(BlockHeader) + bh->size + sizeof(BlockFooter) > heap_end) {
            LOG_ERR("MREDIS [CORRUPTION DETECTED]: [%lu]번 블록 크기가 힙 경계를 초과함. (Size: %lu)",
                    block_count, bh->size);
            return SHM_ERR_CORRUPT;
        }

        // 2. 블록 푸터 포인터 및 대칭성 검사
        BlockFooter *bf = (BlockFooter *)((uint8_t *)bh + sizeof(BlockHeader) + bh->size);

        // 매직 넘버 검증 (Footer)
        if (bf->magic != HEAP_BLOCK_MAGIC) {
            LOG_ERR("MREDIS [CORRUPTION DETECTED]: 블록 [%lu] 푸터 매직 넘버 파손! (Offset: %lu, Magic: 0x%X)",
                    block_count, current_off, bf->magic);
            return SHM_ERR_CORRUPT;
        }

        // 헤더와 푸터의 기록된 사이즈 일치 여부 검증 (대칭성 검사)
        if (bh->size != bf->size) {
            LOG_ERR("MREDIS [CORRUPTION DETECTED]: 블록 [%lu] 헤더-푸터 크기 불일치. (Header: %lu, Footer: %lu)",
                    block_count, bh->size, bf->size);
            return SHM_ERR_CORRUPT;
        }

        // 3. 통계 데이터 검증 유효성 누적
        int is_free = bh->flags & 1u;
        if (!is_free) {
            // 할당된 블록인 경우 메타데이터 바이트 수 누적
            calculated_used_bytes += (bh->size + sizeof(BlockHeader) + sizeof(BlockFooter));
        } else {
            // 프리 블록인 경우 Bin 인덱스가 올바른 범위 내에 유효한지 체크
            uint32_t bin_idx = (bh->flags >> 8) & 0x1Fu;
            if (bin_idx >= BIN_COUNT) {
                LOG_ERR("MREDIS [CORRUPTION DETECTED]: 프리 블록 [%lu]의 Bin 인덱스 범위 초과. (Bin: %u)",
                        block_count, bin_idx);
                return SHM_ERR_CORRUPT;
            }
        }

        // 다음 인접 물리 블록 오프셋으로 이동
        current_off += (sizeof(BlockHeader) + bh->size + sizeof(BlockFooter));
    }

    // 4. 최종 누적 실측치와 헤더 메타데이터 비교 검증
    if (hh->used_bytes != calculated_used_bytes) {
        LOG_WARN("MREDIS [INTEGRITY WARN]: 힙 헤더의 사용량 필드 불일치. (Header: %lu, 실측치: %lu) -> 동기화 수행.",
                 hh->used_bytes, calculated_used_bytes);
        // 복구 가능한 메타데이터는 실측치로 자동 동기화 보정
        hh->used_bytes = calculated_used_bytes;
    }

    LOG_INFO("MREDIS [INTEGRITY PASS]: 총 %lu개 물리 블록 검증 완료. 오염 없음.", block_count);
    return SHM_OK;
}

/* ── 공개 heap API (heap_mutex 직렬화) ──────────────────── */
uint64_t heap_alloc(MRedisHandle *h, uint64_t size)
{
    HeapHeader *hh = core_heap_hdr(h);
    int rc = pthread_mutex_lock(&hh->heap_mutex);
	if ((errno = mredis_recover_and_audit(h, rc)) != SHM_OK) {
        // 힙 헤더 파괴가 확인되면 다른 프로세스들을 보호하기 위해 락을 풀고 강제 리턴
        pthread_mutex_unlock(&hh->heap_mutex);
        return OFFSET_NULL;
    }
    uint64_t off = heap_alloc_locked(h, size);
    pthread_mutex_unlock(&hh->heap_mutex);
    return off;
}
int heap_free(MRedisHandle *h, uint64_t data)
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
    uint64_t off = (sizeof(MRedisHeader) + 7ULL) & ~7ULL;
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

static void heap_init_region(MRedisHandle *h, uint64_t hs, uint64_t hsz)
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

    LOG_INFO("Segregated Fit + Coalescing Heap 초기화 완료 (%lu MB)", hsz >> 20);
}

static void mutex_attr_ps_robust(pthread_mutexattr_t *ma)
{
    pthread_mutexattr_init(ma);
    pthread_mutexattr_setpshared(ma, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(ma,  PTHREAD_MUTEX_ROBUST);
}

MRedisHandle *mredis_create(const char *name, uint64_t size)
{
    LOG_INFO("mredis_create: %s %lu MB", name, size >> 20);
    if (size < (16ULL << 20)) size = 16ULL << 20;

    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0666);
    if (fd < 0) { LOG_ERR("shm_open: %s", strerror(errno)); return NULL; }
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); shm_unlink(name); return NULL; }

    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    if (base == MAP_FAILED) { close(fd); shm_unlink(name); return NULL; }
    memset(base, 0, size);

    MRedisHandle *h = (MRedisHandle *)malloc(sizeof(MRedisHandle));
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

    MRedisHeader *s = (MRedisHeader *)base;
    s->mredis_size       = size;
    s->bucket_offset     = bo;
    s->heap_header_offset = hho;
    s->hash_table_size   = (uint32_t)HASH_TABLE_SIZE;
    s->version           = 5;
    s->initialized       = 0;

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
    LOG_INFO("mredis_create 완료 ver=%u", s->version);
    return h;
}

MRedisHandle *mredis_open_existing(const char *name)
{
    int fd = shm_open(name, O_RDWR, 0666);
    if (fd < 0) { LOG_ERR("mredis_open: %s", strerror(errno)); return NULL; }
    struct stat st; fstat(fd, &st);
    uint64_t size = (uint64_t)st.st_size;
    void *base = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { close(fd); return NULL; }
    if (!((MRedisHeader *)base)->initialized) { munmap(base, size); close(fd); return NULL; }
    MRedisHandle *h = (MRedisHandle *)malloc(sizeof(MRedisHandle));
    if (!h) { munmap(base, size); close(fd); return NULL; }
    h->base = base; h->fd = fd; h->size = size;
    LOG_INFO("mredis_open_existing: %s %lu MB", name, size >> 20);
    return h;
}

void mredis_close(MRedisHandle *h)   {
	if (h) {
		mredis_heap_validate_integrity(h);
		munmap(h->base, h->size);
		close(h->fd); free(h);
	}
}
void mredis_destroy(const char *name) { shm_unlink(name); }

void mredis_dump_stats(MRedisHandle *h)
{
    if (!h) return;
    MRedisHeader *s  = core_mredis_hdr(h);
    HeapHeader *hh = core_heap_hdr(h);
    fprintf(stderr,
            "=== MREDIS SHM 통계 (ver%u) ===\n  SHM=%lu MB  버킷=0x%x\n"
            "  힙=%lu MB  사용=%lu KB  잔여=%lu MB\n  alloc/free=%lu/%lu\n",
            s->version, s->mredis_size >> 20, s->hash_table_size,
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
void bucket_lock(MRedisHandle *h, uint32_t idx)
{
    BucketEntry *bk = core_get_bucket(h, idx);
    int rc = pthread_mutex_lock(&bk->mutex);
    if (rc == EOWNERDEAD) {
        LOG_WARN("버킷[0x%x] mutex 복구", idx);
        pthread_mutex_consistent(&bk->mutex);
    }
}

uint64_t bucket_find_locked(MRedisHandle *h, BucketEntry *bk, const void *key, uint32_t klen, uint32_t tf, uint64_t *op)
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

uint64_t bucket_find(MRedisHandle *h, uint32_t idx,
                      const void *key, uint32_t klen,
                      uint32_t tf, uint64_t *op)
{
    bucket_lock(h, idx);
    BucketEntry *bk = core_get_bucket(h, idx);
    uint64_t ne = bucket_find_locked(h, bk, key, klen, tf, op);
    pthread_mutex_unlock(&bk->mutex);
    return ne;
}

/* [FIXED-3] nameentry_alloc: 내부 락 획득 순서 원자화 및 롤백 로직 메모리 누수 해결 */
uint64_t nameentry_alloc(MRedisHandle *h, const void *key, uint32_t klen,
                          uint32_t type, uint64_t data_off)
{
    if (klen == 0) return OFFSET_NULL; // 방어 코드

    HeapHeader *hh = core_heap_hdr(h);
    uint64_t ne_off = OFFSET_NULL;
    uint64_t ko = OFFSET_NULL;

    // 대형 락(heap_mutex)을 명시적으로 한 번만 획득하여 두 할당의 원자성을 100% 보장
    int rc = pthread_mutex_lock(&hh->heap_mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&hh->heap_mutex);

    ne_off = heap_alloc_locked(h, sizeof(NameEntry));
    if (ne_off != OFFSET_NULL) {
        ko = heap_alloc_locked(h, klen);
        if (ko == OFFSET_NULL) {
            // 원자적 롤백: 락이 걸린 상태에서 즉시 프리
            heap_free_locked(h, ne_off);
            ne_off = OFFSET_NULL;
        }
    }

    pthread_mutex_unlock(&hh->heap_mutex);

    if (ne_off == OFFSET_NULL) {
        return OFFSET_NULL;
    }

    memcpy(OFF2PTR(h, ko), key, klen);
    NameEntry *ne = (NameEntry *)OFF2PTR(h, ne_off);
    ne->next_offset = OFFSET_NULL;
    ne->key_offset  = ko;
    ne->key_len     = klen;
    ne->type        = type;
    ne->data_offset = data_off;
    return ne_off;
}

void nameentry_free(MRedisHandle *h, uint64_t ne_off)
{
    if (ne_off == OFFSET_NULL) return;
    NameEntry *ne = (NameEntry *)OFF2PTR(h, ne_off);
    
    HeapHeader *hh = core_heap_hdr(h);
    int rc = pthread_mutex_lock(&hh->heap_mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&hh->heap_mutex);

    if (ne->key_offset != OFFSET_NULL) {
        heap_free_locked(h, ne->key_offset);
    }
    heap_free_locked(h, ne_off);

    pthread_mutex_unlock(&hh->heap_mutex);
}
