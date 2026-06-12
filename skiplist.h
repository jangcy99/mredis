#ifndef	_SKIPLIST_H_
#define	_SKIPLIST_H_

#define SL_P          0.25
#define SL_MAX_LEVEL  32

typedef struct {
    double   score;
    uint64_t member_offset;
	uint32_t member_len;
	uint32_t level_count;
    uint64_t backward_offset;
    uint64_t forward[SL_MAX_LEVEL];
} SkipNode;

typedef struct {
    pthread_mutex_t mutex;
    uint64_t head_offset;
	uint64_t tail_offset;
	uint64_t length;
    uint32_t cur_level;
	uint32_t pad;
} ZSetHeader;

static inline SkipNode    *core_sn(MRedisHandle *h, uint64_t off) { return (SkipNode*)OFF2PTR(h,off); }
/* ============================================================
 *  Skip List
 * ============================================================ */
uint32_t sl_random_level(void);
int      sl_cmp(double s1,const void*m1,uint32_t ml1,double s2,const void*m2,uint32_t ml2);
uint64_t sl_find_update(MRedisHandle*h,ZSetHeader*z,double sc,const void*m,uint32_t ml,uint64_t upd[SL_MAX_LEVEL]);
uint64_t sl_find_member(MRedisHandle*h,ZSetHeader*z,const void*m,uint32_t ml);
uint64_t sl_node_alloc(MRedisHandle*h,double sc,const void*m,uint32_t ml,uint32_t lv);
void     sl_node_free(MRedisHandle*h,uint64_t n);
void     sl_unlink(MRedisHandle*h,ZSetHeader*z,uint64_t n,uint64_t upd[SL_MAX_LEVEL]);

#endif
