#ifndef SHM_TYPES_H
#define SHM_TYPES_H

/*
 * shm_types.h  –  공통 타입 정의 (v5)
 *
 *  ┌──────────────────────────────────────────────────────────────┐
 *  │  모든 cmd_*() 함수의 반환 타입 = s_replyObject *             │
 *  │  모든 cmd_*() 함수의 시그니처:                               │
 *  │    s_replyObject *cmd_XXX(ShmHandle *h,                      │
 *  │                           string_t   *args[],                │
 *  │                           uint32_t    argc);                 │
 *  │                                                              │
 *  │  args[0] = 커맨드 이름  (예: "SET", "ZADD", "HGET")         │
 *  │  args[1] = 첫 번째 인자 (보통 key)                          │
 *  │  args[2~] = 나머지 인자                                      │
 *  └──────────────────────────────────────────────────────────────┘
 *
 *  s_replyObject.type 코드 (hiredis 호환)
 *    REPLY_STRING   = 1   단일 문자열  (ptr / len)
 *    REPLY_ARRAY    = 2   배열         (element / elements)
 *    REPLY_INTEGER  = 3   정수         (integer)
 *    REPLY_NIL      = 4   null
 *    REPLY_STATUS   = 5   상태 문자열  (ptr / len, 예: "OK")
 *    REPLY_ERROR    = 6   에러 문자열  (ptr / len)
 *    REPLY_DOUBLE   = 7   부동소수     (dval)
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>

/* ============================================================
 *  컴파일 타임 파라미터
 * ============================================================ */
#define SHM_DEFAULT_NAME    "/shm_hashtable"

#ifndef HASH_TABLE_SIZE
#define HASH_TABLE_SIZE     0x1000000ULL    /* 16M 버킷 (4GB 운영) */
#endif

#ifndef HASH_FIELD_BUCKETS
#define HASH_FIELD_BUCKETS  16U
#endif

#define HEAP_MIN_BLOCK      64
#define OFFSET_NULL         UINT64_MAX

/* ============================================================
 *  에러 코드
 * ============================================================ */
#define SHM_OK              0
#define SHM_ERR             (-1)
#define SHM_ERR_KEY_EXISTS  (-2)
#define SHM_ERR_NOT_FOUND   (-3)
#define SHM_ERR_NOMEM       (-4)
#define SHM_ERR_INVAL       (-5)
#define SHM_ERR_OVERFLOW    (-6)
#define SHM_ERR_CORRUPT     (-7)
#define SHM_ERR_TYPE        (-8)
#define SHM_ERR_ARGC        (-9)
#define SHM_ERR_PARSE       (-10)

/* ============================================================
 *  디버그 레벨
 * ============================================================ */
#define DBG_NONE    0
#define DBG_ERROR   1
#define DBG_WARN    2
#define DBG_INFO    3
#define DBG_TRACE   4

#ifndef SHM_DEBUG_LEVEL
#define SHM_DEBUG_LEVEL DBG_INFO
#endif

/* ============================================================
 *  ZSet 파라미터
 * ============================================================ */
#define ZSET_MAX_LEVEL  32
#define ZSET_P          0.25

#define ZADD_NONE   0x00
#define ZADD_NX     0x01
#define ZADD_XX     0x02
#define ZADD_GT     0x04
#define ZADD_LT     0x08
#define ZADD_CH     0x10

#define ZRANGE_ASC  0
#define ZRANGE_DESC 1

/* NameEntry 타입 */
#define ENTRY_KV     1u
#define ENTRY_ZSET   2u
#define ENTRY_HASH   3u
#define ENTRY_ZHSET  4u
#define ENTRY_SET    5u
/* ============================================================
 *  string_t  –  길이 포함 바이트열
 * ============================================================ */
typedef struct {
    const char *ptr;
    uint32_t    len;
} string_t;

/* 힙 할당 string_t (ptr 이 구조체 뒤에 인라인) */
static inline string_t *string_new(const char *p, uint32_t l)
{
    string_t *s = (string_t *)malloc(sizeof(string_t) + l + 1);
    if (!s) return NULL;
    char *buf = (char *)(s + 1);
    if (l > 0 && p) memcpy(buf, p, l);
    buf[l] = '\0';
    s->ptr = buf; s->len = l;
    return s;
}
static inline void string_del(string_t *s) { free(s); }

/* C 문자열 리터럴에서 스택 string_t 초기화 */
#define STR_LIT(s) ((string_t){ .ptr=(s), .len=(uint32_t)(sizeof(s)-1) })

/* ============================================================
 *  s_replyObject  –  모든 커맨드의 반환 타입
 * ============================================================ */

/* type 코드 (hiredis 호환) */
#define REPLY_STRING    1
#define REPLY_ARRAY     2
#define REPLY_INTEGER   3
#define REPLY_NIL       4
#define REPLY_STATUS    5
#define REPLY_ERROR     6
#define REPLY_DOUBLE    7

typedef struct s_replyObject {
    int32_t  type;          /* REPLY_* 코드 */
    int64_t  integer;       /* REPLY_INTEGER */
    double   dval;          /* REPLY_DOUBLE  */
    size_t   len;           /* REPLY_STRING / STATUS / ERROR : ptr 의 바이트 수 */
    void    *ptr;           /* REPLY_STRING / STATUS / ERROR : malloc'd 문자열  */
    size_t   elements;      /* REPLY_ARRAY : 하위 원소 수 */
    struct s_replyObject **element; /* REPLY_ARRAY : 하위 원소 포인터 배열 */
} s_replyObject;

/* ── 생성 헬퍼 ─────────────────────────────────────────────── */

/* STATUS "OK" */
static inline s_replyObject *reply_ok(void)
{
    s_replyObject *r = (s_replyObject *)calloc(1, sizeof(s_replyObject));
    if (!r) return NULL;
    r->type = REPLY_STATUS;
    r->ptr  = strdup("OK");
    r->len  = 2;
    return r;
}

/* NIL */
static inline s_replyObject *reply_nil(void)
{
    s_replyObject *r = (s_replyObject *)calloc(1, sizeof(s_replyObject));
    if (!r) return NULL;
    r->type = REPLY_NIL;
    return r;
}

/* INTEGER */
static inline s_replyObject *reply_integer(int64_t v)
{
    s_replyObject *r = (s_replyObject *)calloc(1, sizeof(s_replyObject));
    if (!r) return NULL;
    r->type    = REPLY_INTEGER;
    r->integer = v;
    return r;
}

/* DOUBLE */
static inline s_replyObject *reply_double(double v)
{
    s_replyObject *r = (s_replyObject *)calloc(1, sizeof(s_replyObject));
    if (!r) return NULL;
    r->type = REPLY_DOUBLE;
    r->dval = v;
    return r;
}

/* STRING (바이트열 복사) */
static inline s_replyObject *reply_string(const char *p, size_t l)
{
    s_replyObject *r = (s_replyObject *)calloc(1, sizeof(s_replyObject));
    if (!r) return NULL;
    r->type = REPLY_STRING;
    r->ptr  = malloc(l + 1);
    if (!r->ptr) { free(r); return NULL; }
    if (l > 0) memcpy(r->ptr, p, l);
    ((char *)r->ptr)[l] = '\0';
    r->len = l;
    return r;
}

/* ERROR */
static inline s_replyObject *reply_error(int errcode, const char *msg)
{
    s_replyObject *r = (s_replyObject *)calloc(1, sizeof(s_replyObject));
    if (!r) return NULL;
    r->type    = REPLY_ERROR;
    r->integer = errcode;      /* 에러 코드 저장 */
    char buf[256];
    int  bl = snprintf(buf, sizeof(buf), "ERR(%d) %s", errcode, msg ? msg : "");
    r->ptr = strdup(buf);
    r->len = (size_t)(bl > 0 ? bl : 0);
    return r;
}

/* ARRAY (빈 배열 생성, reply_array_append 으로 채움) */
static inline s_replyObject *reply_array(size_t cap)
{
    s_replyObject *r = (s_replyObject *)calloc(1, sizeof(s_replyObject));
    if (!r) return NULL;
    r->type     = REPLY_ARRAY;
    r->elements = 0;
    r->element  = cap > 0
                  ? (s_replyObject **)calloc(cap, sizeof(s_replyObject *))
                  : NULL;
    /* cap 을 ptr 에 임시 보관 (realloc 기준) */
    r->ptr = (void *)(uintptr_t)cap;
    return r;
}

/* 배열에 원소 추가 (소유권 이전) */
static inline int reply_array_append(s_replyObject *arr, s_replyObject *child)
{
    if (!arr || arr->type != REPLY_ARRAY || !child) return SHM_ERR_INVAL;
    size_t cap = (size_t)(uintptr_t)arr->ptr;
    if (arr->elements >= cap) {
        size_t new_cap = cap ? cap * 2 : 4;
        s_replyObject **tmp = (s_replyObject **)realloc(
            arr->element, new_cap * sizeof(s_replyObject *));
        if (!tmp) return SHM_ERR_NOMEM;
        arr->element = tmp;
        arr->ptr     = (void *)(uintptr_t)new_cap;
    }
    arr->element[arr->elements++] = child;
    return SHM_OK;
}

/* 재귀 해제 */
static inline void reply_free(s_replyObject *r)
{
    if (!r) return;
    if (r->type == REPLY_ARRAY && r->element) {
        for (size_t i = 0; i < r->elements; i++)
            reply_free(r->element[i]);
        free(r->element);
    } else {
        free(r->ptr);
    }
    free(r);
}

/* 출력 (디버그/테스트용) */
static inline void reply_print(const s_replyObject *r, int indent)
{
    if (!r) { printf("%*s(null)\n", indent, ""); return; }
    switch (r->type) {
    case REPLY_STATUS:
        printf("%*s+%s\n", indent, "", r->ptr ? (char *)r->ptr : ""); break;
    case REPLY_ERROR:
        printf("%*s-%s\n", indent, "", r->ptr ? (char *)r->ptr : ""); break;
    case REPLY_INTEGER:
        printf("%*s:%lld\n", indent, "", (long long)r->integer); break;
    case REPLY_DOUBLE:
        printf("%*s,%.17g\n", indent, "", r->dval); break;
    case REPLY_STRING:
        printf("%*s$%zu \"%.*s\"\n", indent, "", r->len,
               (int)r->len, r->ptr ? (char *)r->ptr : ""); break;
    case REPLY_NIL:
        printf("%*s(nil)\n", indent, ""); break;
    case REPLY_ARRAY:
        printf("%*s*%zu\n", indent, "", r->elements);
        for (size_t i = 0; i < r->elements; i++)
            reply_print(r->element[i], indent + 2);
        break;
    default:
        printf("%*s(unknown type %d)\n", indent, "", r->type); break;
    }
}

/* ============================================================
 *  SHM 내부 구조체
 * ============================================================ */
#define HEAP_BLOCK_MAGIC  0xDEADBEEFU

typedef struct {
	uint64_t head_offset;
	pthread_mutex_t mutex;
} BucketEntry;

typedef struct {
	uint64_t next_offset;
	uint64_t key_offset;
	uint32_t key_len;
	uint32_t type;
	uint64_t data_offset;
} NameEntry;

typedef struct {
	uint64_t val_offset;
	uint32_t val_len;
	uint32_t pad;
} KVNode;

typedef struct {
    double   score;
    uint64_t member_offset; uint32_t member_len; uint32_t level_count;
    uint64_t backward_offset;
    uint64_t forward[ZSET_MAX_LEVEL];
} SkipNode;

typedef struct {
    uint64_t head_offset; uint64_t tail_offset; uint64_t length;
    uint32_t cur_level;   uint32_t pad;
    pthread_mutex_t mutex;
} ZSetHeader;

typedef struct	{
	uint64_t	next;
	uint64_t	str_offset;
	uint32_t	str_len;
	uint32_t	ref_count;
} FieldString;

typedef struct	{
	uint64_t field_buckets[256];
    uint32_t total_fields;
    uint32_t pad;
    pthread_mutex_t mutex;
} FieldPoolHeader;

typedef struct {
    uint64_t next_offset;
	uint64_t field_offset;
    uint64_t val_offset;
	uint32_t field_len;
	uint32_t val_len;
} FieldEntry;

typedef struct {
    uint64_t field_count;
	uint32_t n_buckets;
	uint32_t pad;
    pthread_mutex_t mutex;
    /* 뒤이어 uint64_t field_buckets[n_buckets] */
} HashHeader;

#define BIN_COUNT          32

typedef struct BlockHeader {
    uint64_t size;       /* 반드시 유지되는 데이터 크기 */
    uint64_t next;       /* free list only */
    uint32_t magic;
    uint32_t flags;
} BlockHeader;


typedef struct {
    uint64_t heap_start;
    uint64_t heap_size;
    uint64_t free_bins[BIN_COUNT];        /* ← Best-Fit Bin 배열 (free_list 대신) */
    pthread_mutex_t heap_mutex;
    uint64_t total_alloc;
    uint64_t total_free;
    uint64_t used_bytes;
} HeapHeader;

typedef struct {
    uint64_t shm_size;
	uint64_t bucket_offset;
	uint64_t heap_header_offset;
//	uint64_t field_pool_offset;
    uint32_t initialized;
	uint32_t version;
	uint32_t hash_table_size;
	uint32_t pad;
	FieldPoolHeader	field_pool;
} ShmHeader;

typedef struct {
	void *base;
	int fd;
	uint64_t size;
} ShmHandle;

/* ============================================================
 *  Set (Unordered Set)
 * ============================================================ */
typedef struct SetEntry {
    uint64_t next_offset;
    uint64_t member_offset;
    uint32_t member_len;
    uint32_t pad;
} SetEntry;

typedef struct {
    uint64_t member_count;
    uint32_t n_buckets;
    uint32_t pad;
    pthread_mutex_t mutex;
    /* 뒤이어 uint64_t member_buckets[n_buckets] */
} SetHeader;
#endif /* SHM_TYPES_H */

