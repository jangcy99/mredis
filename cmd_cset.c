/*
 * cmd_cset.c  –  Chunk Sorted Set (CSET) 구현
 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include "mredis_types.h"
#include "mredis_core.h"
#include "cmd_cset.h"

/* §1 rwlock 헬퍼 */
static void cs_rwlock_init(pthread_rwlock_t *rw) {
    pthread_rwlockattr_t a;
    pthread_rwlockattr_init(&a);
    pthread_rwlockattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    pthread_rwlock_init(rw, &a);
    pthread_rwlockattr_destroy(&a);
}
static inline void cs_rdlock(CSetHeader *h) { pthread_rwlock_rdlock(&h->rwlock); }
static inline void cs_wrlock(CSetHeader *h) { pthread_rwlock_wrlock(&h->rwlock); }
static inline void cs_unlock(CSetHeader *h) { pthread_rwlock_unlock(&h->rwlock); }

/* §2 청크 포인터 헬퍼 */
static inline CSetChunk *cs_chunk(MRedisHandle *h, uint64_t off) {
    return (off == OFFSET_NULL) ? NULL : (CSetChunk *)OFF2PTR(h, off);
}
static uint64_t cs_chunk_alloc(MRedisHandle *h) {
    uint64_t off = heap_alloc(h, sizeof(CSetChunk));
    if (off == OFFSET_NULL) return OFFSET_NULL;
    CSetChunk *c = cs_chunk(h, off);
    memset(c, 0, sizeof(CSetChunk));
    c->prev_off = OFFSET_NULL;
    c->next_off = OFFSET_NULL;
    return off;
}

/* §3 청크 내 이진 탐색 */
static uint32_t cs_lb(const CSetChunk *c, uint64_t score) {
    uint32_t lo=0, hi=c->count;
    while(lo<hi){ uint32_t mid=lo+(hi-lo)/2;
        if(c->entries[mid].score<score) lo=mid+1; else hi=mid; }
    return lo;
}
static uint32_t cs_ub(const CSetChunk *c, uint64_t score) __attribute__((unused));
static uint32_t cs_ub(const CSetChunk *c, uint64_t score) {
    uint32_t lo=0, hi=c->count;
    while(lo<hi){ uint32_t mid=lo+(hi-lo)/2;
        if(c->entries[mid].score<=score) lo=mid+1; else hi=mid; }
    return lo;
}

/* §4 BSetHeader 생명주기 */
static int cset_type_check(MRedisHandle *h, BucketEntry *bk,
                             const void *key, uint32_t klen, uint64_t *on) {
    uint64_t ne = bucket_find_locked(h, bk, key, klen, 0, NULL);
    if (ne==OFFSET_NULL) { if(on)*on=OFFSET_NULL; return 1; }
    NameEntry *nep=(NameEntry*)OFF2PTR(h,ne);
    if(nep->type!=ENTRY_CSET) return SHM_ERR_KEY_EXISTS;
    if(on)*on=ne;
    return 0;
}

static int cset_create_locked(MRedisHandle *h, BucketEntry *bk,
                                const void *key, uint32_t klen) {
    uint64_t bh_off = heap_alloc(h, sizeof(CSetHeader));
    if(bh_off==OFFSET_NULL) return SHM_ERR_NOMEM;
    CSetHeader *bh=(CSetHeader*)OFF2PTR(h,bh_off);
    memset(bh,0,sizeof(CSetHeader));
    cs_rwlock_init(&bh->rwlock);
    bh->chunk_head=bh->chunk_tail=OFFSET_NULL;

    uint64_t c0=cs_chunk_alloc(h);
    if(c0==OFFSET_NULL){ pthread_rwlock_destroy(&bh->rwlock); heap_free(h,bh_off); return SHM_ERR_NOMEM; }
    bh->chunk_head=bh->chunk_tail=c0; bh->chunk_count=1;

    uint64_t ne=nameentry_alloc(h,key,klen,ENTRY_CSET,bh_off);
    if(ne==OFFSET_NULL){ heap_free(h,c0); pthread_rwlock_destroy(&bh->rwlock); heap_free(h,bh_off); return SHM_ERR_NOMEM; }
    ((NameEntry*)OFF2PTR(h,ne))->next_offset=bk->head_offset;
    bk->head_offset=ne;
    return SHM_OK;
}

static CSetHeader *cset_get_or_create(MRedisHandle *h, const void *key, uint32_t klen, int *err) {
    CSetHeader *bh=core_cset_get(h,key,klen);
    if(bh){ if(err)*err=SHM_OK; return bh; }
    uint32_t     idx = mredis_hash(key, klen)%((MRedisHeader*)(h->base))->hash_table_size;
    BucketEntry *bk=core_get_bucket(h,idx);
    bucket_lock(h,idx);
    uint64_t ne=OFFSET_NULL;
    int chk=cset_type_check(h,bk,key,klen,&ne);
    if(chk==SHM_ERR_KEY_EXISTS){ pthread_mutex_unlock(&bk->mutex); if(err)*err=SHM_ERR_KEY_EXISTS; return NULL; }
    if(chk==0){ bh=(CSetHeader*)OFF2PTR(h,((NameEntry*)OFF2PTR(h,ne))->data_offset);
        pthread_mutex_unlock(&bk->mutex); if(err)*err=SHM_OK; return bh; }
    int r=cset_create_locked(h,bk,key,klen);
    pthread_mutex_unlock(&bk->mutex);
    if(r!=SHM_OK){ if(err)*err=r; return NULL; }
    if(err)*err=SHM_OK;
    return core_cset_get(h,key,klen);
}

CSetHeader *core_cset_get(MRedisHandle *h, const void *key, uint32_t klen) {
    uint32_t     idx = mredis_hash(key, klen)%((MRedisHeader*)(h->base))->hash_table_size;
    uint64_t ne=bucket_find(h,idx,key,klen,ENTRY_CSET,NULL);
    if(ne==OFFSET_NULL) return NULL;
    return (CSetHeader*)OFF2PTR(h,((NameEntry*)OFF2PTR(h,ne))->data_offset);
}

/*
 * §5  cs_bits_insert  — deleted_bits 를 pos 위치에서 오른쪽으로 1칸 shift
 *
 * [FIX-B6] int32_t 루프 — uint32_t w==0 언더플로 방지
 * [FIX-B6+] pos >= CSET_CHUNK 경계 방어 — pw >= CSET_DEL_WORDS 방지
 *
 * 동작:
 *   pos 이후의 모든 비트를 오른쪽으로 1칸 밀어 빈 슬롯을 만든다.
 *   마지막으로 CS_DEL_CLR 으로 삽입 위치 비트를 0(live)으로 초기화.
 */
static void cs_bits_insert(CSetChunk *c, uint32_t pos) {
    /* [B6+] pos 는 반드시 0 ~ CSET_CHUNK-1 범위여야 함 */
    if (pos >= CSET_CHUNK) return;

    int32_t words = (int32_t)CSET_DEL_WORDS;      /* 16 */
    int32_t pw    = (int32_t)(pos / 64);           /* 0 ~ 15 */

    /* [B6] int32_t 루프 — w==0 일 때 w-- 가 -1 이 되어 루프 종료 (uint32_t UINT32_MAX wrap 방지) */
    for (int32_t w = words - 1; w > pw; w--)
        c->deleted_bits[w] = (c->deleted_bits[w] << 1)
                           | (c->deleted_bits[w - 1] >> 63);

    /* pw 에 해당하는 word 내부 shift: pos%64 위치에서 상위 비트를 1칸 이동 */
    uint64_t mask = (1ULL << (pos % 64)) - 1;
    uint64_t hi   = c->deleted_bits[pw] & ~mask;
    c->deleted_bits[pw] = (c->deleted_bits[pw] & mask) | (hi << 1);

    CS_DEL_CLR(c, pos);   /* 삽입 위치 = live(0) */
}

/*
 * §6 청크 병합 (wrlock 구간 내)
 *
 * [B1]  병합 조건: dst->count + src->live <= CSET_CHUNK
 *       (live 합만 보면 dst dead 슬롯 때문에 배열 경계 초과 가능)
 * [B1+] 루프 내 dst->count 상한 방어 (continue)
 * [B2]  dst dead 슬롯 재활성 시: src 원소가 이동 + dst dead 부활
 *       → net 효과: total_count 는 이동 전 그대로 (src에서 -1, dst에서 +1 = 0)
 *       → B2 경로에서 total_count 변경 불필요 (기존 +1 은 오류)
 * [B3]  pos >= dst->count 분기를 명확한 {} 블록으로 작성
 * [B4]  src heap_free 전 total_count -= src->live
 *       (src live 원소들이 dst로 이동하면 src 청크가 사라지므로)
 *       단, B2 dead 재활성 경로에서는 dst 원소 수가 증가하지 않으므로
 *       total_count 에서 src->live 를 차감해야 정확함
 *       → 최종: total_count -= src->live (모든 이동 원소 차감)
 *                total_count += B2_activated (dst dead 재활성 수)
 * [B5]  CSetEntry 값 복사(by value) — memmove 후 stale 포인터 방지
 * [B7]  min/max_score 갱신을 루프 완료 후 수행
 */
static void cs_try_merge(MRedisHandle *h, CSetHeader *bh, uint64_t c_off) {
    CSetChunk *c = cs_chunk(h, c_off);
    if (!c || c->live >= CSET_MERGE_THRESH) return;

    CSetChunk *prev = cs_chunk(h, c->prev_off);
    CSetChunk *next = cs_chunk(h, c->next_off);

    /* [B1] 병합 조건: dst->count(dead 포함) + src->live <= CSET_CHUNK
     *         → dst 에 src live 원소가 들어갈 슬롯이 있어야 함
     *
     * prev 방향: src=c, dst=prev → dst(prev)->count + src(c)->live <= CHUNK
     * next 방향: src=next, dst=c → dst(c)->count + src(next)->live <= CHUNK
     *
     * [B1 FIX] next 방향 조건 수정:
     *   기존: next->count + c->live   (src의 count + dst의 live → 잘못된 방향)
     *   수정: c->count    + next->live (dst의 count + src의 live → 올바른 방향)
     *
     * 또한 live=0 인 청크가 dst 가 되면 dead 슬롯으로 꽉 찬 경우
     * 공간 = CSET_CHUNK - dst->count 이므로 조건에 포함됨.
     */
    uint64_t src_off = OFFSET_NULL, dst_off = OFFSET_NULL;
    if (prev && (prev->count + c->live) <= CSET_CHUNK) {
        /* src=c(작은쪽), dst=prev → prev 에 c 흡수 */
        src_off = c_off;      dst_off = c->prev_off;
    } else if (next && (c->count + next->live) <= CSET_CHUNK) {
        /* src=next, dst=c → c 에 next 흡수: c->count(dead포함) + next->live */
        src_off = c->next_off; dst_off = c_off;
    }
    if (src_off == OFFSET_NULL) return;

    CSetChunk *src = cs_chunk(h, src_off);
    CSetChunk *dst = cs_chunk(h, dst_off);

    for (uint32_t i = 0; i < src->count; i++) {
        if (CS_DEL_GET(src, i)) continue;

        /* [B5] 포인터가 아닌 값 복사 — memmove 후 stale 방지 */
        CSetEntry e = src->entries[i];

        uint32_t pos = cs_lb(dst, e.score);

        if (pos < dst->count && dst->entries[pos].score == e.score) {
            if (!CS_DEL_GET(dst, pos)) {
                /* 살아있는 슬롯: value 교체 */
                heap_free(h, dst->entries[pos].offset);
                dst->entries[pos].offset = e.offset;
                dst->entries[pos].vlen   = e.vlen;
            } else {
                /* [B2] dead 슬롯 재활성
                 *  src 원소(이미 total_count에 포함) → dst dead 슬롯 덮어씀
                 *  dst dead 슬롯은 이전 CDEL 로 total_count-- 됐던 원소
                 *  net 효과: src -1 (B4에서 처리) + dst +1 = 0 → total_count 변화 없음
                 *  여기서는 dst->live++ 만 수행 */
                CS_DEL_CLR(dst, pos);
                dst->entries[pos].offset = e.offset;
                dst->entries[pos].vlen   = e.vlen;
                dst->live++;
                /* total_count 변경 없음: src->live 차감은 B4(아래)에서 일괄 처리 */
            }
        } else {
            /* [B3] 명확한 분기 블록 + [B1+] 배열 경계 최종 방어 */
            if (dst->count >= CSET_CHUNK) {
                /* dst 가 꽉 찬 경우 — 이론상 B1 조건에서 막혀야 하지만
                 * 연속 병합 시 누적될 수 있으므로 방어적으로 건너뜀 */
                continue;
            }
            if (pos < dst->count) {
                memmove(&dst->entries[pos + 1], &dst->entries[pos],
                        (dst->count - pos) * sizeof(CSetEntry));
                cs_bits_insert(dst, pos);
            } else {
                /* pos == dst->count: 끝에 추가, 해당 비트는 이미 0 */
                CS_DEL_CLR(dst, pos);
            }
            dst->entries[pos] = e;
            dst->count++;
            dst->live++;
        }
    }

    /* [B7] min/max 갱신은 루프 완료 후 한 번만 */
    if (dst->count > 0) {
        dst->min_score = dst->entries[0].score;
        dst->max_score = dst->entries[dst->count - 1].score;
    }

    /* src 체인에서 제거
     * [B4] cs_try_merge 는 원소를 삭제하지 않고 청크 간에 이동하므로
     *      total_count 변경 없음. total_count 는 CSET(+1)/CDEL(-1)/CPOP(-1) 만으로 관리.
     *      B2(dead 재활성) 도 마찬가지: src 원소가 dst dead 슬롯을 덮으면
     *      dst dead 는 이미 CDEL 로 --됐으므로 재활성으로 +1 해야 하지만,
     *      src 원소가 사라지므로 -1 → net 0 → total_count 변화 없음 */

    CSetChunk *sp = cs_chunk(h, src->prev_off);
    CSetChunk *sn = cs_chunk(h, src->next_off);
    if (sp) sp->next_off = src->next_off; else bh->chunk_head = src->next_off;
    if (sn) sn->prev_off = src->prev_off; else bh->chunk_tail = src->prev_off;
    heap_free(h, src_off);
    bh->chunk_count--;
}

/* §7 단일 삽입 (wrlock 내) */
static int cs_insert_one(MRedisHandle *h, CSetHeader *bh,
                          uint64_t score, const void *val, uint32_t vlen) {
    uint64_t vo=heap_alloc(h,vlen>0?vlen:1);
    if(vo==OFFSET_NULL) return SHM_ERR_NOMEM;
    if(vlen>0) memcpy(OFF2PTR(h,vo),val,vlen);

    /* Fast-Path: tail 끝 추가 */
    CSetChunk *tail=cs_chunk(h,bh->chunk_tail);
    if(tail && (tail->count==0 || score>tail->max_score)) {
        if(tail->count<CSET_CHUNK){
            uint32_t idx=tail->count;
            tail->entries[idx].score=score; tail->entries[idx].offset=vo; tail->entries[idx].vlen=vlen;
            CS_DEL_CLR(tail,idx); tail->count++; tail->live++;
            tail->max_score=score; if(tail->count==1) tail->min_score=score;
            bh->total_count++; return 1;
        }
        /* 새 청크 */
        uint64_t nc_off=cs_chunk_alloc(h);
        if(nc_off==OFFSET_NULL){ heap_free(h,vo); return SHM_ERR_NOMEM; }
        CSetChunk *nc=cs_chunk(h,nc_off);
        nc->entries[0].score=score; nc->entries[0].offset=vo; nc->entries[0].vlen=vlen;
        nc->count=1; nc->live=1; nc->min_score=nc->max_score=score;
        nc->prev_off=bh->chunk_tail; nc->next_off=OFFSET_NULL;
        tail->next_off=nc_off; bh->chunk_tail=nc_off; bh->chunk_count++;
        bh->total_count++; return 1;
    }

    /* 일반: 청크 탐색 */
    uint64_t cur_off=bh->chunk_head;
    CSetChunk *cur=NULL;
    while(cur_off!=OFFSET_NULL){
        cur=cs_chunk(h,cur_off);
        if(score<=cur->max_score || cur->next_off==OFFSET_NULL) break;
        cur_off=cur->next_off;
    }
    if(!cur){ heap_free(h,vo); return SHM_ERR_NOMEM; }

    uint32_t pos=cs_lb(cur,score);

    /* 동일 score */
    if(pos<cur->count && cur->entries[pos].score==score){
        if(CS_DEL_GET(cur,pos)){
            CS_DEL_CLR(cur,pos); cur->entries[pos].offset=vo; cur->entries[pos].vlen=vlen;
            cur->live++; bh->total_count++; return 1;
        }
        heap_free(h,cur->entries[pos].offset); cur->entries[pos].offset=vo; cur->entries[pos].vlen=vlen;
        return 0;
    }

    /* 청크 꽉 참 → 분할 */
    if(cur->count>=CSET_CHUNK){
        uint64_t nc_off=cs_chunk_alloc(h);
        if(nc_off==OFFSET_NULL){ heap_free(h,vo); return SHM_ERR_NOMEM; }
        CSetChunk *nc=cs_chunk(h,nc_off);
        uint32_t half=CSET_CHUNK/2;
        memcpy(nc->entries,&cur->entries[half],half*sizeof(CSetEntry));
        for(uint32_t i=0;i<half;i++){
            if(CS_DEL_GET(cur,half+i)) CS_DEL_SET(nc,i); else CS_DEL_CLR(nc,i); }
        nc->count=half; nc->live=0;
        for(uint32_t i=0;i<half;i++) if(!CS_DEL_GET(nc,i)) nc->live++;
        /* cur 뒤 절반 초기화 */
        for(uint32_t i=half;i<CSET_CHUNK;i++) CS_DEL_CLR(cur,i);
        cur->count=half; cur->live=0;
        for(uint32_t i=0;i<half;i++) if(!CS_DEL_GET(cur,i)) cur->live++;
        cur->max_score=cur->entries[cur->count-1].score;
        nc->min_score=nc->entries[0].score; nc->max_score=nc->entries[nc->count-1].score;
        nc->prev_off=cur_off; nc->next_off=cur->next_off;
        if(cur->next_off!=OFFSET_NULL) cs_chunk(h,cur->next_off)->prev_off=nc_off;
        else bh->chunk_tail=nc_off;
        cur->next_off=nc_off; bh->chunk_count++;
        if(score>=nc->min_score){ cur_off=nc_off; cur=nc; pos=cs_lb(cur,score); }
    }

    /* memmove + 비트 shift */
    if(pos<cur->count){
        memmove(&cur->entries[pos+1],&cur->entries[pos],(cur->count-pos)*sizeof(CSetEntry));
        cs_bits_insert(cur,pos);
    } else CS_DEL_CLR(cur,pos);
    cur->entries[pos].score=score; cur->entries[pos].offset=vo; cur->entries[pos].vlen=vlen;
    cur->count++; cur->live++;
    if(cur->count==1||score<cur->min_score) cur->min_score=score;
    if(score>cur->max_score) cur->max_score=score;
    bh->total_count++; return 1;
}

/* §8 스캔 공통 헬퍼 */
typedef struct {
    int      use_index;
    int64_t  start_idx, stop_idx;
    uint64_t score_min, score_max;
    int64_t  limit_off, limit_cnt;
} CScanParam;

static s_replyObject *cs_scan(MRedisHandle *h, CSetHeader *bh, CScanParam *p) {
    cs_rdlock(bh);
    uint64_t snap_head=bh->chunk_head;
    cs_unlock(bh);

    s_replyObject *arr=reply_array(32);
    if(!arr) return reply_error(SHM_ERR_NOMEM,mredis_strerror(SHM_ERR_NOMEM));

    int64_t global_idx=0, skipped=0, collected=0;
    uint64_t cur_off=snap_head;
    while(cur_off!=OFFSET_NULL){
        CSetChunk *c=cs_chunk(h,cur_off);
        if(!p->use_index && c->count>0 && c->min_score>p->score_max) break;
        for(uint32_t i=0;i<c->count;i++){
            if(CS_DEL_GET(c,i)) continue;
            CSetEntry *e=&c->entries[i];
            if(p->use_index){
                if(global_idx>p->stop_idx) goto done;
                if(global_idx>=p->start_idx){
                    char sb[24]; int sl=snprintf(sb,sizeof(sb),"%llu",(unsigned long long)e->score);
                    reply_array_append(arr,reply_string(sb,(size_t)(sl>0?sl:0)));
                    const char *vp=e->vlen>0?(const char*)OFF2PTR(h,e->offset):"";
                    reply_array_append(arr,reply_string(vp,e->vlen)); collected++;
                }
                global_idx++;
            } else {
                if(e->score<p->score_min) continue;
                if(e->score>p->score_max) goto done;
                if(skipped<p->limit_off){ skipped++; continue; }
                if(p->limit_cnt>=0 && collected>=p->limit_cnt) goto done;
                char sb[24]; int sl=snprintf(sb,sizeof(sb),"%llu",(unsigned long long)e->score);
                reply_array_append(arr,reply_string(sb,(size_t)(sl>0?sl:0)));
                const char *vp=e->vlen>0?(const char*)OFF2PTR(h,e->offset):"";
                reply_array_append(arr,reply_string(vp,e->vlen)); collected++;
            }
        }
        cur_off=c->next_off;
    }
done:
    if(arr->elements==0){ reply_free(arr); return reply_array(0); }
    return arr;
}

/* §9 커맨드 구현 */

s_replyObject *cmd_cset(MRedisHandle *h, string_t *args[], uint32_t argc) {
    if(argc<4||(argc-2)%2!=0) return reply_error(SHM_ERR_ARGC,"usage: CSET key score value [score value ...]");
    const void *key=args[1]->ptr; uint32_t klen=args[1]->len;
    if(!klen) return reply_error(SHM_ERR_INVAL,"key 비어있음");
    {
		uint32_t     idx = mredis_hash(key, klen)%((MRedisHeader*)(h->base))->hash_table_size;
		BucketEntry *bk=core_get_bucket(h,idx);
        bucket_lock(h,idx);
        uint64_t any=bucket_find_locked(h,bk,key,klen,0,NULL);
        if(any!=OFFSET_NULL && ((NameEntry*)OFF2PTR(h,any))->type!=ENTRY_CSET){
            pthread_mutex_unlock(&bk->mutex); return reply_error(SHM_ERR_KEY_EXISTS,mredis_strerror(SHM_ERR_KEY_EXISTS)); }
        pthread_mutex_unlock(&bk->mutex);
    }
    int err=SHM_OK;
    CSetHeader *bh=cset_get_or_create(h,key,klen,&err);
    if(!bh) return reply_error(err,mredis_strerror(err));
    cs_wrlock(bh); int64_t added=0;
    for(uint32_t i=2;i+1<argc;i+=2){
        char *ep=NULL; uint64_t score=strtoull(args[i]->ptr,&ep,10);
        if(ep==args[i]->ptr){ cs_unlock(bh); return reply_error(SHM_ERR_PARSE,"score 변환 실패"); }
        int r=cs_insert_one(h,bh,score,args[i+1]->ptr,args[i+1]->len);
        if(r<0){ cs_unlock(bh); return reply_error(r,mredis_strerror(r)); }
        if(r==1) added++;
    }
    cs_unlock(bh); return reply_integer(added);
}

s_replyObject *cmd_cget(MRedisHandle *h, string_t *args[], uint32_t argc) {
    if(argc<3) return reply_error(SHM_ERR_ARGC,"usage: CGET key score");
    CSetHeader *bh=core_cset_get(h,args[1]->ptr,args[1]->len);
    if(!bh) return reply_nil();
    char *ep=NULL; uint64_t score=strtoull(args[2]->ptr,&ep,10);
    if(ep==args[2]->ptr) return reply_error(SHM_ERR_PARSE,"score 변환 실패");
    cs_rdlock(bh); uint64_t snap=bh->chunk_head; cs_unlock(bh);
    uint64_t cur_off=snap;
    while(cur_off!=OFFSET_NULL){
        CSetChunk *c=cs_chunk(h,cur_off);
        if(c->count>0&&score<c->min_score) break;
        if(c->count>0&&score>c->max_score){ cur_off=c->next_off; continue; }
        uint32_t pos=cs_lb(c,score);
        if(pos<c->count&&c->entries[pos].score==score){
            if(CS_DEL_GET(c,pos)) return reply_nil();
            const char *vp=c->entries[pos].vlen>0?(const char*)OFF2PTR(h,c->entries[pos].offset):"";
            return reply_string(vp,c->entries[pos].vlen);
        }
        cur_off=c->next_off;
    }
    return reply_nil();
}

s_replyObject *cmd_cdel(MRedisHandle *h, string_t *args[], uint32_t argc) {
    if(argc<3) return reply_error(SHM_ERR_ARGC,"usage: CDEL key score [score ...]");
    CSetHeader *bh=core_cset_get(h,args[1]->ptr,args[1]->len);
    if(!bh) return reply_integer(0);
    cs_wrlock(bh); int64_t removed=0;
    for(uint32_t a=2;a<argc;a++){
        char *ep=NULL; uint64_t score=strtoull(args[a]->ptr,&ep,10);
        if(ep==args[a]->ptr) continue;
        uint64_t cur_off=bh->chunk_head;
        while(cur_off!=OFFSET_NULL){
            CSetChunk *c=cs_chunk(h,cur_off);
            if(c->count>0&&score>c->max_score){ cur_off=c->next_off; continue; }
            if(c->count>0&&score<c->min_score) break;
            uint32_t pos=cs_lb(c,score);
            if(pos<c->count&&c->entries[pos].score==score&&!CS_DEL_GET(c,pos)){
                CS_DEL_SET(c,pos); heap_free(h,c->entries[pos].offset);
                c->entries[pos].offset=OFFSET_NULL; c->live--; bh->total_count--; removed++;
                cs_try_merge(h,bh,cur_off);
            }
            break;
        }
    }
    cs_unlock(bh); return reply_integer(removed);
}

s_replyObject *cmd_crange(MRedisHandle *h, string_t *args[], uint32_t argc) {
    if(argc<4) return reply_error(SHM_ERR_ARGC,"usage: CRANGE key start stop");
    CSetHeader *bh=core_cset_get(h,args[1]->ptr,args[1]->len);
    if(!bh) return reply_array(0);
    cs_rdlock(bh); int64_t len=(int64_t)bh->total_count; cs_unlock(bh);
    if(len==0) return reply_array(0);
    int64_t start=strtoll(args[2]->ptr,NULL,10), stop=strtoll(args[3]->ptr,NULL,10);
    if(start<0) start=len+start;
    if(stop<0)  stop =len+stop;
    if(start<0) start=0;
    if(stop>=len) stop=len-1;
    if(start>stop) return reply_array(0);
    CScanParam p={0}; p.use_index=1; p.start_idx=start; p.stop_idx=stop;
    return cs_scan(h,bh,&p);
}

s_replyObject *cmd_crangebyscore(MRedisHandle *h, string_t *args[], uint32_t argc) {
    if(argc<4) return reply_error(SHM_ERR_ARGC,"usage: CRANGEBYSCORE key min max [LIMIT offset count]");
    CScanParam p={0}; p.use_index=0;
    p.score_min=strtoull(args[2]->ptr,NULL,10); p.score_max=strtoull(args[3]->ptr,NULL,10);
    p.limit_off=0; p.limit_cnt=-1;
    for(uint32_t i=4;i<argc;i++)
        if(!strcasecmp(args[i]->ptr,"LIMIT")&&i+2<argc){
            p.limit_off=strtoll(args[i+1]->ptr,NULL,10); p.limit_cnt=strtoll(args[i+2]->ptr,NULL,10); i+=2; }
    if(p.score_min>p.score_max) return reply_array(0);
    CSetHeader *bh=core_cset_get(h,args[1]->ptr,args[1]->len);
    if(!bh) return reply_array(0);
    return cs_scan(h,bh,&p);
}

s_replyObject *cmd_ccard(MRedisHandle *h, string_t *args[], uint32_t argc) {
    if(argc<2) return reply_error(SHM_ERR_ARGC,"usage: CCARD key");
    CSetHeader *bh=core_cset_get(h,args[1]->ptr,args[1]->len);
    if(!bh) return reply_integer(0);
    cs_rdlock(bh); int64_t cnt=(int64_t)bh->total_count; cs_unlock(bh);
    return reply_integer(cnt);
}

s_replyObject *cmd_crank(MRedisHandle *h, string_t *args[], uint32_t argc) {
    if(argc<3) return reply_error(SHM_ERR_ARGC,"usage: CRANK key score");
    CSetHeader *bh=core_cset_get(h,args[1]->ptr,args[1]->len);
    if(!bh) return reply_nil();
    char *ep=NULL; uint64_t score=strtoull(args[2]->ptr,&ep,10);
    if(ep==args[2]->ptr) return reply_error(SHM_ERR_PARSE,"score 변환 실패");
    cs_rdlock(bh); uint64_t snap=bh->chunk_head; cs_unlock(bh);
    int64_t rank=0; uint64_t cur_off=snap;
    while(cur_off!=OFFSET_NULL){
        CSetChunk *c=cs_chunk(h,cur_off);
        if(c->count>0&&score<c->min_score) break;
        for(uint32_t i=0;i<c->count;i++){
            if(CS_DEL_GET(c,i)) continue;
            if(c->entries[i].score==score) return reply_integer(rank);
            if(c->entries[i].score<score) rank++;
            else goto not_found;
        }
        cur_off=c->next_off;
    }
not_found: return reply_nil();
}

s_replyObject *cmd_ccount(MRedisHandle *h, string_t *args[], uint32_t argc) {
    if(argc<4) return reply_error(SHM_ERR_ARGC,"usage: CCOUNT key min max");
    uint64_t mn=strtoull(args[2]->ptr,NULL,10), mx=strtoull(args[3]->ptr,NULL,10);
    if(mn>mx) return reply_integer(0);
    CSetHeader *bh=core_cset_get(h,args[1]->ptr,args[1]->len);
    if(!bh) return reply_integer(0);
    cs_rdlock(bh); uint64_t snap=bh->chunk_head; cs_unlock(bh);
    int64_t cnt=0; uint64_t cur_off=snap;
    while(cur_off!=OFFSET_NULL){
        CSetChunk *c=cs_chunk(h,cur_off);
        if(c->count>0&&c->min_score>mx) break;
        for(uint32_t i=0;i<c->count;i++){
            if(CS_DEL_GET(c,i)) continue;
            if(c->entries[i].score>=mn&&c->entries[i].score<=mx) cnt++;
        }
        cur_off=c->next_off;
    }
    return reply_integer(cnt);
}

static s_replyObject *cpop_impl(MRedisHandle *h, string_t *args[], uint32_t argc, int from_min) {
    if(argc<2) return reply_error(SHM_ERR_ARGC,from_min?"usage: CPOPMIN key [count]":"usage: CPOPMAX key [count]");
    int64_t count=1;
    if(argc>=3){ count=strtoll(args[2]->ptr,NULL,10); if(count<=0) return reply_error(SHM_ERR_INVAL,"count>0 필요"); }
    CSetHeader *bh=core_cset_get(h,args[1]->ptr,args[1]->len);
    if(!bh) return reply_array(0);
    cs_wrlock(bh);
    if(count>(int64_t)bh->total_count) count=(int64_t)bh->total_count;
    s_replyObject *arr=reply_array((size_t)(count*2));
    if(!arr){ cs_unlock(bh); return reply_error(SHM_ERR_NOMEM,mredis_strerror(SHM_ERR_NOMEM)); }
    for(int64_t n=0;n<count&&bh->total_count>0;n++){
        uint64_t c_off=from_min?bh->chunk_head:bh->chunk_tail;
        CSetChunk *c=cs_chunk(h,c_off); int32_t slot=-1;
        if(from_min){ for(uint32_t i=0;i<c->count&&slot<0;i++) if(!CS_DEL_GET(c,i)) slot=(int32_t)i; }
        else{ for(int32_t i=(int32_t)c->count-1;i>=0&&slot<0;i--) if(!CS_DEL_GET(c,i)) slot=i; }
        if(slot<0) break;
        CSetEntry *e=&c->entries[slot];
        char sb[24]; int sl=snprintf(sb,sizeof(sb),"%llu",(unsigned long long)e->score);
        reply_array_append(arr,reply_string(sb,(size_t)(sl>0?sl:0)));
        const char *vp=e->vlen>0?(const char*)OFF2PTR(h,e->offset):"";
        reply_array_append(arr,reply_string(vp,e->vlen));
        CS_DEL_SET(c,(uint32_t)slot); heap_free(h,e->offset); e->offset=OFFSET_NULL;
        c->live--; bh->total_count--; cs_try_merge(h,bh,c_off);
    }
    cs_unlock(bh); return arr;
}
s_replyObject *cmd_cpopmin(MRedisHandle *h,string_t *a[],uint32_t n){return cpop_impl(h,a,n,1);}
s_replyObject *cmd_cpopmax(MRedisHandle *h,string_t *a[],uint32_t n){return cpop_impl(h,a,n,0);}

s_replyObject *cmd_cdrop(MRedisHandle *h, string_t *args[], uint32_t argc) {
    if(argc<2) return reply_error(SHM_ERR_ARGC,"usage: CDROP key");
    const void *key=args[1]->ptr; uint32_t klen=args[1]->len;
    uint32_t     idx = mredis_hash(key, klen)%((MRedisHeader*)(h->base))->hash_table_size;
	BucketEntry *bk=core_get_bucket(h,idx);
    bucket_lock(h,idx);
    uint64_t prev=OFFSET_NULL;
    uint64_t ne_off=bucket_find_locked(h,bk,key,klen,ENTRY_CSET,&prev);
    if(ne_off==OFFSET_NULL){ pthread_mutex_unlock(&bk->mutex); return reply_error(SHM_ERR_NOT_FOUND,mredis_strerror(SHM_ERR_NOT_FOUND)); }
    NameEntry *ne=(NameEntry*)OFF2PTR(h,ne_off);
    uint64_t bh_off=ne->data_offset; CSetHeader *bh=(CSetHeader*)OFF2PTR(h,bh_off);
    if(prev==OFFSET_NULL) bk->head_offset=ne->next_offset;
    else ((NameEntry*)OFF2PTR(h,prev))->next_offset=ne->next_offset;
    nameentry_free(h,ne_off); pthread_mutex_unlock(&bk->mutex);
    cs_wrlock(bh);
    uint64_t cur_off=bh->chunk_head;
    while(cur_off!=OFFSET_NULL){
        CSetChunk *c=cs_chunk(h,cur_off); uint64_t nxt=c->next_off;
        for(uint32_t i=0;i<c->count;i++) if(!CS_DEL_GET(c,i)&&c->entries[i].offset!=OFFSET_NULL) heap_free(h,c->entries[i].offset);
        heap_free(h,cur_off); cur_off=nxt;
    }
    cs_unlock(bh); pthread_rwlock_destroy(&bh->rwlock); heap_free(h,bh_off);
    return reply_ok();
}

s_replyObject *cmd_ccompact(MRedisHandle *h, string_t *args[], uint32_t argc) {
    if(argc<2) return reply_error(SHM_ERR_ARGC,"usage: CCOMPACT key");
    CSetHeader *bh=core_cset_get(h,args[1]->ptr,args[1]->len);
    if(!bh) return reply_error(SHM_ERR_NOT_FOUND,mredis_strerror(SHM_ERR_NOT_FOUND));
    cs_wrlock(bh); int64_t removed=0;
    uint64_t cur_off=bh->chunk_head;
    while(cur_off!=OFFSET_NULL){
        CSetChunk *c=cs_chunk(h,cur_off); uint64_t nxt=c->next_off;
        if(c->count==c->live){ cur_off=nxt; continue; }
        uint32_t wr=0;
        for(uint32_t rd=0;rd<c->count;rd++){
            if(CS_DEL_GET(c,rd)){ removed++; continue; }
            if(wr!=rd) c->entries[wr]=c->entries[rd];
            wr++;
        }
        memset(c->deleted_bits,0,sizeof(c->deleted_bits));
        c->count=wr; c->live=wr;
        if(wr>0){ c->min_score=c->entries[0].score; c->max_score=c->entries[wr-1].score; }
        if(c->count==0){
            uint64_t p_off=c->prev_off, n_off=c->next_off;
            CSetChunk *pc=cs_chunk(h,p_off), *nc=cs_chunk(h,n_off);
            if(pc) pc->next_off=n_off; else bh->chunk_head=n_off;
            if(nc) nc->prev_off=p_off; else bh->chunk_tail=p_off;
            heap_free(h,cur_off); bh->chunk_count--;
        }
        cur_off=nxt;
    }
    if(bh->chunk_head==OFFSET_NULL){
        uint64_t nc=cs_chunk_alloc(h);
        if(nc!=OFFSET_NULL){ bh->chunk_head=bh->chunk_tail=nc; bh->chunk_count=1; }
    }
    cs_unlock(bh); return reply_integer(removed);
}

/* ── ENTRY_CSET drop ────────────────────────────────────────
 *  청크 체인 전체 해제 (삭제 비트 무시, 모든 live offset heap_free)
 * ─────────────────────────────────────────────────────────── */
int drop_cset(MRedisHandle *h, const void *key, uint32_t klen)
{
    uint32_t     idx = mredis_hash(key, klen)%((MRedisHeader*)(h->base))->hash_table_size;
    BucketEntry *bk  = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t prev   = OFFSET_NULL;
    uint64_t ne_off = bucket_find_locked(h, bk, key, klen, ENTRY_CSET, &prev);
    if (ne_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return SHM_ERR_NOT_FOUND;
    }
    NameEntry  *ne    = (NameEntry *)OFF2PTR(h, ne_off);
    uint64_t    bh_off = ne->data_offset;
    CSetHeader *bh     = (CSetHeader *)OFF2PTR(h, bh_off);

    if (prev == OFFSET_NULL) bk->head_offset = ne->next_offset;
    else ((NameEntry *)OFF2PTR(h, prev))->next_offset = ne->next_offset;
    nameentry_free(h, ne_off);
    pthread_mutex_unlock(&bk->mutex);

    pthread_rwlock_wrlock(&bh->rwlock);
    uint64_t cur_off = bh->chunk_head;
    while (cur_off != OFFSET_NULL) {
        CSetChunk *c = (CSetChunk *)OFF2PTR(h, cur_off);
        uint64_t   nxt = c->next_off;
        for (uint32_t i = 0; i < c->count; i++)
            if (!CS_DEL_GET(c,i) && c->entries[i].offset != OFFSET_NULL)
                heap_free(h, c->entries[i].offset);
        heap_free(h, cur_off);
        cur_off = nxt;
    }
    pthread_rwlock_unlock(&bh->rwlock);
    pthread_rwlock_destroy(&bh->rwlock);
    heap_free(h, bh_off);
    LOG_TRACE("DEL(CSET): '%.*s'", klen, (const char *)key);
    return SHM_OK;
}

