#include "mredis_types.h"
#include "mredis_core.h"
#include "skiplist.h"

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
    while (lv < SL_MAX_LEVEL) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        if ((rng & 0xFFFF) < (uint32_t)(0xFFFF * SL_P)) lv++;
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

uint64_t sl_find_update(MRedisHandle *h, ZSetHeader *zsh,
                         double sc, const void *m, uint32_t ml,
                         uint64_t upd[SL_MAX_LEVEL])
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

uint64_t sl_find_member(MRedisHandle *h, ZSetHeader *zsh,
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

uint64_t sl_node_alloc(MRedisHandle *h, double sc,
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
    for (int i = 0; i < SL_MAX_LEVEL; i++) sn->forward[i] = OFFSET_NULL;
    return n;
}

void sl_node_free(MRedisHandle *h, uint64_t n)
{
    if (n == OFFSET_NULL) return;
    heap_free(h, core_sn(h, n)->member_offset);
    heap_free(h, n);
}

void sl_unlink(MRedisHandle *h, ZSetHeader *zsh,
               uint64_t n_off, uint64_t upd[SL_MAX_LEVEL])
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
