/*
 * test_all.c  –  mredis 전체 통합 테스트
 *
 * 커버 범위:
 *   §01  KV  : SET / GET / 갱신 / 빈값 / 타입충돌
 *   §02  KEYS: 패턴 매칭
 *   §03  DEL : KV·ZSET·HASH 혼합 / 없는키 / 중복키
 *   §04  ZSET: ZCREATE/ZDROP/ZADD/ZREM/ZSCORE/ZINCRBY
 *              ZRANK/ZREVRANK/ZCARD/ZCOUNT/ZRANGE/ZRANGEBYSCORE
 *              ZPOPMIN/ZPOPMAX / NX·XX·GT·LT·CH 플래그
 *   §05  HASH: HCREATE/HDROP/HSET/HGET/HDEL/HEXISTS/HLEN
 *              HGETALL/HKEYS/HVALS/HINCRBY/HINCRBYFLOAT
 *   §06  직렬화(serialize) 검증
 *              - bucket_mutex: 동일 버킷에 동시 SET → 손실 없음
 *              - zset_mutex  : 동시 ZADD → length 정확
 *              - hash_mutex  : 동시 HSET → field_count 정확
 *              - heap_mutex  : 동시 alloc/free → 손상 없음
 *   §07  ZINCRBY TOCTOU 수정 검증 (단일 mutex 구간)
 *   §08  HLEN mutex 보호 검증
 *   §09  cmd_kv SET: 갱신 시 new-alloc-then-free 순서 검증
 *   §10  멀티프로세스 stress (8 proc × 500 iter)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>

#include "shm_types.h"
#include "shm_core.h"
#include "cmd_dispatch.h"
#include "cmd_bset.h"
#include "cmd_del.h"

/* ─── 테스트 픽스처 ────────────────────────────────────── */
#define SHM_NAME "/shm_mredis_test"
#define PASS     "\033[32m[PASS]\033[0m"
#define FAIL     "\033[31m[FAIL]\033[0m"
#define SECT(t)  printf("\n\033[36m══ %s ══\033[0m\n", (t))

static int g_pass = 0, g_fail = 0;
#define CHECK(c,m) do { \
    if(c){printf(PASS " %s\n",(m));g_pass++;} \
    else {printf(FAIL " %s  [L%d]\n",(m),__LINE__);g_fail++;} \
} while(0)

/* ─── cmd_dispatch 래퍼 ────────────────────────────────── */
static s_replyObject *run(ShmHandle *h, ...)
{
    string_t *args[64]; string_t bufs[64]; uint32_t argc = 0;
    va_list ap; va_start(ap, h);
    while (argc < 64) {
        const char *s = va_arg(ap, const char *);
        if (!s) break;
        bufs[argc].ptr = s;
        bufs[argc].len = (uint32_t)strlen(s);
        args[argc]     = &bufs[argc];
        argc++;
    }
    va_end(ap);
    return cmd_dispatch(h, args, argc);
}

/* ─── 응답 타입 헬퍼 ───────────────────────────────────── */
static int is_ok (s_replyObject *r)
    { return r && r->type==REPLY_STATUS && strcmp((char*)r->ptr,"OK")==0; }
static int is_int(s_replyObject *r, int64_t v)
    { return r && r->type==REPLY_INTEGER && r->integer==v; }
static int is_str(s_replyObject *r, const char *s)
    { return r && r->type==REPLY_STRING  && strcmp((char*)r->ptr,s)==0; }
static int is_nil(s_replyObject *r)
    { return r && r->type==REPLY_NIL; }
static int is_err(s_replyObject *r)
    { return r && r->type==REPLY_ERROR; }
static int arr_sz(s_replyObject *r, size_t n)
    { return r && r->type==REPLY_ARRAY && r->elements==n; }
static int arr_has(s_replyObject *r, const char *s) {
    if (!r || r->type!=REPLY_ARRAY) return 0;
    for (size_t i=0;i<r->elements;i++)
        if (r->element[i] && r->element[i]->type==REPLY_STRING
            && strcmp((char*)r->element[i]->ptr,s)==0) return 1;

    return 0;
}

/* ARRAY i번째 = 문자열 s */
static int arr_str(s_replyObject *r,size_t i,const char *s){
    return r&&r->type==REPLY_ARRAY&&i<r->elements
        &&r->element[i]&&r->element[i]->type==REPLY_STRING
        &&strcmp((char*)r->element[i]->ptr,s)==0; }

/* ============================================================
 *  §01  KV
 * ============================================================ */
static void t01_kv(ShmHandle *h)
{
    SECT("01. KV – SET / GET / 갱신 / 빈값 / 타입충돌");
    s_replyObject *r;

    r=run(h,"SET","k1","hello",NULL); CHECK(is_ok(r),"SET → OK"); reply_free(r);
    r=run(h,"GET","k1",NULL);         CHECK(is_str(r,"hello"),"GET=hello"); reply_free(r);

    /* 갱신 [FIX-1: new-alloc-then-free] */
    r=run(h,"SET","k1","world",NULL); CHECK(is_ok(r),"SET 갱신 → OK"); reply_free(r);
    r=run(h,"GET","k1",NULL);         CHECK(is_str(r,"world"),"GET 갱신=world"); reply_free(r);

    /* 빈 값 */
    r=run(h,"SET","empty","",NULL);   CHECK(is_ok(r),"SET 빈값 → OK"); reply_free(r);
    r=run(h,"GET","empty",NULL);      CHECK(r&&r->type==REPLY_STRING&&r->len==0,"GET 빈값=empty"); reply_free(r);

    /* 없는 키 */
    r=run(h,"GET","nokey",NULL); CHECK(is_nil(r),"GET 없는키→NIL"); reply_free(r);

    /* 빈 key 거부 */
    r=run(h,"SET","","v",NULL); CHECK(is_err(r),"SET 빈key→ERR"); reply_free(r);

    /* 타입 충돌: ZADD 후 SET */
    r=run(h,"ZADD","ztype","1","m",NULL); reply_free(r);
    r=run(h,"SET","ztype","v",NULL); CHECK(is_err(r),"SET→ZSET key ERR"); reply_free(r);

    /* 인자 부족 */
    r=run(h,"SET","k",NULL); CHECK(is_err(r),"SET 인자부족→ERR"); reply_free(r);
}

/* ============================================================
 *  §02  KEYS
 * ============================================================ */
static void t02_keys(ShmHandle *h)
{
    SECT("02. KEYS – 패턴 매칭 [FIX: 버킷 mutex 보호]");
    s_replyObject *r;

    r=run(h,"SET","key:a","1",NULL); reply_free(r);
    r=run(h,"SET","key:b","2",NULL); reply_free(r);
    r=run(h,"SET","other","3",NULL); reply_free(r);

    r=run(h,"KEYS","key:*",NULL);
    CHECK(r&&r->type==REPLY_ARRAY, "KEYS key:* → ARRAY");
    CHECK(arr_has(r,"key:a"), "KEYS 포함 key:a");
    CHECK(arr_has(r,"key:b"), "KEYS 포함 key:b");
    CHECK(!arr_has(r,"other"),"KEYS 미포함 other");
    reply_free(r);

    r=run(h,"KEYS","*",NULL);
    CHECK(r&&r->type==REPLY_ARRAY&&r->elements>=3,"KEYS * ≥ 3개"); reply_free(r);

    r=run(h,"KEYS","none_match_xyz",NULL);
    CHECK(arr_sz(r,0),"KEYS 미매칭→0"); reply_free(r);

    /* 인자 부족 */
    r=run(h,"KEYS",NULL); CHECK(is_err(r),"KEYS 인자없음→ERR [FIX-2]"); reply_free(r);
}

/* ============================================================
 *  §03  DEL 라우팅
 * ============================================================ */
static void t03_del(ShmHandle *h)
{
    SECT("03. DEL – 타입별 라우팅 / 혼합 / 없는키 / 중복");
    s_replyObject *r;

    r=run(h,"SET",  "dkv","v",          NULL); reply_free(r);
    r=run(h,"ZADD", "dzs","1","m",       NULL); reply_free(r);
    r=run(h,"HSET", "dhs","f","v",       NULL); reply_free(r);

    /* DEL KV */
    r=run(h,"DEL","dkv",NULL);
    CHECK(is_int(r,1),"DEL KV→1"); reply_free(r);
    r=run(h,"GET","dkv",NULL); CHECK(is_nil(r),"DEL 후 GET→NIL"); reply_free(r);

    /* DEL ZSET */
    r=run(h,"DEL","dzs",NULL);
    CHECK(is_int(r,1),"DEL ZSET→1"); reply_free(r);
    r=run(h,"ZCARD","dzs",NULL); CHECK(is_int(r,0),"DEL 후 ZCARD=0"); reply_free(r);

    /* DEL HASH */
    r=run(h,"DEL","dhs",NULL);
    CHECK(is_int(r,1),"DEL HASH→1"); reply_free(r);
    r=run(h,"HLEN","dhs",NULL); CHECK(is_int(r,0),"DEL 후 HLEN=0"); reply_free(r);

    /* 혼합 */
    r=run(h,"SET","mk","v",NULL); reply_free(r);
	r=run(h,"ZADD","mz","1","a",NULL); reply_free(r);
	r=run(h,"HSET","mh","f","v",NULL);reply_free(r);
    r=run(h,"DEL","mk","mz","mh",NULL);
    CHECK(is_int(r,3),"DEL 혼합 3→3"); reply_free(r);

    /* 없는 키 */
    r=run(h,"DEL","nokey",NULL); CHECK(is_int(r,0),"DEL 없는키→0"); reply_free(r);

    /* 중복 키 */
    r=run(h,"SET","dup","v",NULL); reply_free(r);
    r=run(h,"DEL","dup","dup",NULL);
    CHECK(is_int(r,1),"DEL 중복→1(두번째없음)"); reply_free(r);

    /* 라우팅 테이블 항목 */
    size_t cnt=0; del_route_table_get(&cnt);
    CHECK(cnt>=3,"라우팅 테이블 ≥ 3항목");

    /* DEL 후 재생성 (KV→ZSET→HASH 순환) */
    r=run(h,"SET","reuse","kv",NULL); reply_free(r);
    r=run(h,"DEL","reuse",NULL); reply_free(r);
    r=run(h,"ZADD","reuse","10","m",NULL); CHECK(r&&r->type==REPLY_INTEGER,"재생성 ZADD"); reply_free(r);
    reply_free(run(h,"DEL","reuse",NULL));
    r=run(h,"HSET","reuse","f","v",NULL);  CHECK(r&&r->type==REPLY_INTEGER,"재생성 HSET"); reply_free(r);
    reply_free(run(h,"DEL","reuse",NULL));
}

/* ============================================================
 *  §04  ZSET
 * ============================================================ */
static void t04_zset(ShmHandle *h)
{
    SECT("04. ZSET");
    s_replyObject *r;

    /* ZCREATE / ZDROP */
    r=run(h,"ZCREATE","z1",NULL); CHECK(is_ok(r),"ZCREATE→OK"); reply_free(r);
    r=run(h,"ZDROP",  "z1",NULL); CHECK(is_ok(r),"ZDROP→OK");   reply_free(r);
    r=run(h,"ZDROP",  "z1",NULL); CHECK(is_err(r),"ZDROP 없는→ERR"); reply_free(r);

    /* ZADD 기본 */
    r=run(h,"ZADD","zs","100","Alice",NULL); CHECK(is_int(r,1),"ZADD Alice→1"); reply_free(r);
    r=run(h,"ZADD","zs","200","Bob",  NULL); CHECK(is_int(r,1),"ZADD Bob→1");   reply_free(r);
    r=run(h,"ZADD","zs","150","Carol",NULL); CHECK(is_int(r,1),"ZADD Carol→1"); reply_free(r);

    /* ZCARD */
    r=run(h,"ZCARD","zs",NULL); CHECK(is_int(r,3),"ZCARD=3"); reply_free(r);

    /* ZSCORE */
    r=run(h,"ZSCORE","zs","Alice",NULL);
    CHECK(r&&r->type==REPLY_STRING&&fabs(atof((char*)r->ptr)-100.0)<1e-9,"ZSCORE Alice=100"); reply_free(r);
    r=run(h,"ZSCORE","zs","noone",NULL); CHECK(is_nil(r),"ZSCORE 없음→NIL"); reply_free(r);

    /* ZRANK / ZREVRANK (Alice<Carol<Bob) */
    r=run(h,"ZRANK",   "zs","Alice",NULL); CHECK(is_int(r,0),"ZRANK Alice=0");    reply_free(r);
    r=run(h,"ZRANK",   "zs","Bob",  NULL); CHECK(is_int(r,2),"ZRANK Bob=2");      reply_free(r);
    r=run(h,"ZREVRANK","zs","Alice",NULL); CHECK(is_int(r,2),"ZREVRANK Alice=2"); reply_free(r);
    r=run(h,"ZRANK",   "zs","x",    NULL); CHECK(is_nil(r),"ZRANK 없음→NIL");    reply_free(r);

    /* ZCOUNT */
    r=run(h,"ZCOUNT","zs","100","200",NULL); CHECK(is_int(r,3),"ZCOUNT 100~200=3"); reply_free(r);
    r=run(h,"ZCOUNT","zs","100","149",NULL); CHECK(is_int(r,1),"ZCOUNT 100~149=1"); reply_free(r);

    /* ZRANGE */
    r=run(h,"ZRANGE","zs","0","2",NULL);
    CHECK(arr_sz(r,6),"ZRANGE 0 2 크기=6");
    CHECK(r&&r->elements>=1&&r->element[0]&&strcmp((char*)r->element[0]->ptr,"Alice")==0,"ZRANGE[0]=Alice");
    reply_free(r);

    r=run(h,"ZRANGE","zs","0","2","REV",NULL);
    CHECK(r&&r->type==REPLY_ARRAY&&r->elements>=1
          &&strcmp((char*)r->element[0]->ptr,"Bob")==0,"ZRANGE REV[0]=Bob");
    reply_free(r);

    /* 음수 인덱스 */
    r=run(h,"ZRANGE","zs","-1","-1",NULL);
    CHECK(r&&r->type==REPLY_ARRAY&&r->elements==2
          &&strcmp((char*)r->element[0]->ptr,"Bob")==0,"ZRANGE -1-1=Bob");
    reply_free(r);

    /* ZRANGEBYSCORE */
    r=run(h,"ZRANGEBYSCORE","zs","100","150",NULL);
    CHECK(arr_sz(r,4),"ZRANGEBYSCORE 100~150=4"); reply_free(r);
    r=run(h,"ZRANGEBYSCORE","zs","100","200","LIMIT","0","2",NULL);
    CHECK(arr_sz(r,4),"ZRANGEBYSCORE LIMIT 2=4"); reply_free(r);

    /* NX: 있으면 무시 */
    r=run(h,"ZADD","zs","NX","999","Alice",NULL);
    CHECK(is_int(r,0),"ZADD NX 기존→0"); reply_free(r);
    r=run(h,"ZSCORE","zs","Alice",NULL);
    CHECK(r&&fabs(atof((char*)r->ptr)-100.0)<1e-9,"NX 후 score 변경없음"); reply_free(r);

    /* XX: 없으면 무시 */
    r=run(h,"ZADD","zs","XX","999","NewMember",NULL);
    CHECK(is_int(r,0),"ZADD XX 없는→0"); reply_free(r);
    r=run(h,"ZCARD","zs",NULL); CHECK(is_int(r,3),"XX 후 ZCARD=3"); reply_free(r);

    /* GT: score 가 클 때만 갱신 */
    r=run(h,"ZADD","zs","GT","50","Alice",NULL);  /* 50 < 100 → 무시 */
    CHECK(is_int(r,0),"ZADD GT 더작으면→0"); reply_free(r);
    r=run(h,"ZADD","zs","GT","300","Alice",NULL); /* 300 > 100 → 갱신 */
    CHECK(is_int(r,0),"ZADD GT 더크면 changed=0(CH없음)"); reply_free(r);
    r=run(h,"ZSCORE","zs","Alice",NULL);
    CHECK(r&&fabs(atof((char*)r->ptr)-300.0)<1e-9,"GT 후 score=300"); reply_free(r);

    /* CH 플래그 */
    r=run(h,"ZADD","zs","CH","500","Alice",NULL);
    CHECK(is_int(r,1),"ZADD CH 변경→1"); reply_free(r);

    /* ZINCRBY [FIX-1: TOCTOU 없음] */
    r=run(h,"ZINCRBY","zs","10","Bob",NULL);
    CHECK(r&&r->type==REPLY_STRING&&fabs(atof((char*)r->ptr)-210.0)<1e-9,"ZINCRBY Bob=210"); reply_free(r);

    /* ZREM */
    r=run(h,"ZREM","zs","Carol",NULL); CHECK(is_int(r,1),"ZREM Carol→1"); reply_free(r);
    r=run(h,"ZCARD","zs",NULL);        CHECK(is_int(r,2),"ZREM 후 ZCARD=2"); reply_free(r);

    /* ZPOPMIN / ZPOPMAX */
    reply_free(run(h,"ZADD","zpk","1","a","2","b","3","c",NULL));
    r=run(h,"ZPOPMIN","zpk",NULL);
    CHECK(arr_sz(r,2)&&r->element[0]&&strcmp((char*)r->element[0]->ptr,"a")==0,"ZPOPMIN=a"); reply_free(r);
    r=run(h,"ZPOPMAX","zpk","2",NULL);
    CHECK(arr_sz(r,4)&&r->element[0]&&strcmp((char*)r->element[0]->ptr,"c")==0,"ZPOPMAX 2: c 먼저"); reply_free(r);
    r=run(h,"ZCARD","zpk",NULL); CHECK(is_int(r,0),"ZPOPMAX 후 ZCARD=0"); reply_free(r);
}

/* ============================================================
 *  §05  HASH
 * ============================================================ */
static void t05_hash(ShmHandle *h)
{
    SECT("05. HASH");
    s_replyObject *r;

    /* HCREATE / HDROP */
    r=run(h,"HCREATE","h1",NULL); CHECK(is_ok(r),"HCREATE→OK"); reply_free(r);
    r=run(h,"HDROP",  "h1",NULL); CHECK(is_ok(r),"HDROP→OK");   reply_free(r);
    r=run(h,"HDROP",  "h1",NULL); CHECK(is_err(r),"HDROP 없는→ERR"); reply_free(r);

    /* HSET 멀티 */
    r=run(h,"HSET","user","name","Alice","age","30","city","Seoul",NULL);
    CHECK(is_int(r,3),"HSET 3 field→3"); reply_free(r);

    /* HGET */
    r=run(h,"HGET","user","name",NULL); CHECK(is_str(r,"Alice"),"HGET name=Alice"); reply_free(r);
    r=run(h,"HGET","user","age", NULL); CHECK(is_str(r,"30"),   "HGET age=30");     reply_free(r);
    r=run(h,"HGET","user","nope",NULL); CHECK(is_nil(r),"HGET 없는field→NIL");      reply_free(r);

    /* HSET 갱신 */
    r=run(h,"HSET","user","name","Bob",NULL);
    CHECK(is_int(r,0),"HSET 갱신→0"); reply_free(r);
    r=run(h,"HGET","user","name",NULL); CHECK(is_str(r,"Bob"),"HGET 갱신=Bob"); reply_free(r);

    /* HLEN [FIX-4: mutex 보호] */
    r=run(h,"HLEN","user",NULL); CHECK(is_int(r,3),"HLEN=3"); reply_free(r);

    /* HEXISTS */
    r=run(h,"HEXISTS","user","age",  NULL); CHECK(is_int(r,1),"HEXISTS age=1");  reply_free(r);
    r=run(h,"HEXISTS","user","nope", NULL); CHECK(is_int(r,0),"HEXISTS nope=0"); reply_free(r);

    /* HDEL */
    r=run(h,"HDEL","user","age","city",NULL); CHECK(is_int(r,2),"HDEL 2→2"); reply_free(r);
    r=run(h,"HLEN","user",NULL);              CHECK(is_int(r,1),"HDEL 후 HLEN=1"); reply_free(r);

    /* HGETALL / HKEYS / HVALS [FIX-5: cap 조정] */
    reply_free(run(h,"HSET","hh","f1","v1","f2","v2","f3","v3",NULL));
    r=run(h,"HGETALL","hh",NULL); CHECK(arr_sz(r,6),"HGETALL=6"); reply_free(r);
    r=run(h,"HKEYS",  "hh",NULL); CHECK(arr_sz(r,3),"HKEYS=3");   reply_free(r);
    r=run(h,"HVALS",  "hh",NULL); CHECK(arr_sz(r,3),"HVALS=3");   reply_free(r);

    /* HINCRBY */
    reply_free(run(h,"HSET","cnt","n","10",NULL));
    r=run(h,"HINCRBY","cnt","n","5",NULL); CHECK(is_int(r,15),"HINCRBY 10+5=15"); reply_free(r);
    r=run(h,"HINCRBY","cnt","n","-3",NULL); CHECK(is_int(r,12),"HINCRBY 15-3=12"); reply_free(r);

    /* HINCRBYFLOAT */
    reply_free(run(h,"HSET","flt","x","1.5",NULL));
    r=run(h,"HINCRBYFLOAT","flt","x","0.5",NULL);
    CHECK(r&&r->type==REPLY_STRING&&fabs(atof((char*)r->ptr)-2.0)<1e-9,"HINCRBYFLOAT=2.0"); reply_free(r);

    /* 타입 충돌 */
    r=run(h,"HSET","ztype","f","v",NULL); CHECK(is_err(r),"HSET→ZSET key ERR"); reply_free(r);
}

/* ============================================================
 *  §06  직렬화(serialize) 검증 – 멀티스레드
 * ============================================================ */
#define MT_THREADS   8
#define MT_OPS_PER   200

typedef struct { ShmHandle *h; int id; } MtArg;

/* bucket_mutex: 동일 버킷에 동시 SET */
static void *mt_kv_worker(void *arg)
{
    MtArg *a = (MtArg *)arg;
    char k[32], v[32];
    for (int i = 0; i < MT_OPS_PER; i++) {
        snprintf(k, sizeof(k), "mt_kv_%d_%d", a->id, i);
        snprintf(v, sizeof(v), "val_%d_%d",   a->id, i);
        string_t sk={k,(uint32_t)strlen(k)}, sv={v,(uint32_t)strlen(v)};
        string_t sc={"SET",3};
        string_t *args[]={&sc,&sk,&sv};
        s_replyObject *r = cmd_dispatch(a->h, args, 3);
        reply_free(r);
    }
    return NULL;
}

/* zset_mutex: 동시 ZADD */
static void *mt_zset_worker(void *arg)
{
    MtArg *a = (MtArg *)arg;
    char m[32], sc[16];
    for (int i = 0; i < MT_OPS_PER; i++) {
        snprintf(m,  sizeof(m),  "mt_m_%d_%d", a->id, i);
        snprintf(sc, sizeof(sc), "%d", a->id * 1000 + i);
        string_t sk={"mt_zset",7}, ss={sc,(uint32_t)strlen(sc)}, sm={m,(uint32_t)strlen(m)};
        string_t scmd={"ZADD",4};
        string_t *args[]={&scmd,&sk,&ss,&sm};
        s_replyObject *r = cmd_dispatch(a->h, args, 4);
        reply_free(r);
    }
    return NULL;
}

/* hash_mutex: 동시 HSET */
static void *mt_hash_worker(void *arg)
{
    MtArg *a = (MtArg *)arg;
    char f[32], v[32];
    for (int i = 0; i < MT_OPS_PER; i++) {
        snprintf(f, sizeof(f), "f_%d_%d", a->id, i);
        snprintf(v, sizeof(v), "v_%d_%d", a->id, i);
        string_t sk={"mt_hash",7}, sf={f,(uint32_t)strlen(f)}, sv2={v,(uint32_t)strlen(v)};
        string_t scmd={"HSET",4};
        string_t *args[]={&scmd,&sk,&sf,&sv2};
        s_replyObject *r = cmd_dispatch(a->h, args, 4);
        reply_free(r);
    }
    return NULL;
}

static void t06_serialize(ShmHandle *h)
{
    SECT("06. 직렬화 검증 (멀티스레드)");
    pthread_t tids[MT_THREADS];
    MtArg     args[MT_THREADS];

    /* bucket_mutex: MT_THREADS × MT_OPS_PER KV SET → 전체 키 존재 확인 */
    for (int i = 0; i < MT_THREADS; i++) {
        args[i].h = h; args[i].id = i;
        pthread_create(&tids[i], NULL, mt_kv_worker, &args[i]);
    }
    for (int i = 0; i < MT_THREADS; i++) pthread_join(tids[i], NULL);

    int kv_ok = 1;
    for (int i = 0; i < MT_THREADS && kv_ok; i++) {
        for (int j = 0; j < MT_OPS_PER && kv_ok; j++) {
            char k[32]; snprintf(k, sizeof(k), "mt_kv_%d_%d", i, j);
            string_t sk={k,(uint32_t)strlen(k)}, sc={"GET",3};
            string_t *a[]={&sc,&sk};
            s_replyObject *r = cmd_dispatch(h, a, 2);
            if (!r || r->type != REPLY_STRING) kv_ok = 0;
            reply_free(r);
        }
    }
    CHECK(kv_ok, "bucket_mutex: 동시 SET 모두 저장됨");

    /* zset_mutex: MT_THREADS × MT_OPS_PER ZADD → ZCARD 정확 */
    for (int i = 0; i < MT_THREADS; i++) {
        args[i].h = h; args[i].id = i;
        pthread_create(&tids[i], NULL, mt_zset_worker, &args[i]);
    }
    for (int i = 0; i < MT_THREADS; i++) pthread_join(tids[i], NULL);
    s_replyObject *r = run(h, "ZCARD", "mt_zset", NULL);
    CHECK(is_int(r, (int64_t)(MT_THREADS * MT_OPS_PER)),
          "zset_mutex: ZCARD = 스레드×ops");
    reply_free(r);

    /* hash_mutex: MT_THREADS × MT_OPS_PER HSET → HLEN 정확 */
    for (int i = 0; i < MT_THREADS; i++) {
        args[i].h = h; args[i].id = i;
        pthread_create(&tids[i], NULL, mt_hash_worker, &args[i]);
    }
    for (int i = 0; i < MT_THREADS; i++) pthread_join(tids[i], NULL);
    r = run(h, "HLEN", "mt_hash", NULL);
    CHECK(is_int(r, (int64_t)(MT_THREADS * MT_OPS_PER)),
          "hash_mutex: HLEN = 스레드×ops [FIX-4]");
    reply_free(r);
}

/* ============================================================
 *  §07  ZINCRBY TOCTOU 수정 검증
 * ============================================================ */
#define ZINCRBY_THREADS  8
#define ZINCRBY_ITER     100

static void *zincrby_worker(void *arg)
{
    ShmHandle *h = (ShmHandle *)arg;
    for (int i = 0; i < ZINCRBY_ITER; i++) {
        string_t sk={"toctou_z",8}, sd={"1",1}, sm={"counter",7};
        string_t sc={"ZINCRBY",7};
        string_t *args[]={&sc,&sk,&sd,&sm};
        s_replyObject *r = cmd_dispatch(h, args, 4);
        reply_free(r);
    }
    return NULL;
}

static void t07_zincrby_toctou(ShmHandle *h)
{
    SECT("07. ZINCRBY TOCTOU 수정 검증 [FIX-1]");
    pthread_t tids[ZINCRBY_THREADS];
    for (int i = 0; i < ZINCRBY_THREADS; i++)
        pthread_create(&tids[i], NULL, zincrby_worker, h);
    for (int i = 0; i < ZINCRBY_THREADS; i++) pthread_join(tids[i], NULL);

    s_replyObject *r = run(h, "ZSCORE", "toctou_z", "counter", NULL);
    int64_t expected = (int64_t)(ZINCRBY_THREADS * ZINCRBY_ITER);
    int ok = (r && r->type == REPLY_STRING &&
              llabs((int64_t)round(atof((char*)r->ptr)) - expected) == 0);
    CHECK(ok, "ZINCRBY 동시 incr: 최종값 정확");
    if (!ok && r && r->type == REPLY_STRING)
        printf("  기대=%lld 실제=%s\n", (long long)expected, (char*)r->ptr);
    reply_free(r);
}

/* ============================================================
 *  §08  HLEN mutex 보호 검증
 * ============================================================ */
#define HLEN_THREADS  8
#define HLEN_ITER     100

static void *hlen_hset_worker(void *arg)
{
    ShmHandle *h = (ShmHandle *)arg;
    static _Atomic int counter = 0;
    for (int i = 0; i < HLEN_ITER; i++) {
        int id = __atomic_fetch_add(&counter, 1, __ATOMIC_SEQ_CST);
        char f[32], v[16];
        snprintf(f, sizeof(f), "hlen_f_%d", id);
        snprintf(v, sizeof(v), "v%d", id);
        string_t sk={"hlen_key",8}, sf={f,(uint32_t)strlen(f)},sv={v,(uint32_t)strlen(v)};
        string_t sc={"HSET",4};
        string_t *args[]={&sc,&sk,&sf,&sv};
        s_replyObject *r = cmd_dispatch(h, args, 4);
        reply_free(r);
    }
    return NULL;
}

static void t08_hlen_mutex(ShmHandle *h)
{
    SECT("08. HLEN mutex 보호 검증 [FIX-4]");
    pthread_t tids[HLEN_THREADS];
    for (int i = 0; i < HLEN_THREADS; i++)
        pthread_create(&tids[i], NULL, hlen_hset_worker, h);
    for (int i = 0; i < HLEN_THREADS; i++) pthread_join(tids[i], NULL);

    s_replyObject *r = run(h, "HLEN", "hlen_key", NULL);
    int64_t expected = (int64_t)(HLEN_THREADS * HLEN_ITER);
    CHECK(is_int(r, expected), "HLEN 동시 HSET 후 정확한 카운트");
    reply_free(r);
}

/* ============================================================
 *  §09  cmd_kv SET 갱신 alloc-before-free 검증
 * ============================================================ */
static void t09_set_update_safety(ShmHandle *h)
{
    SECT("09. SET 갱신: new-alloc-then-free 안전성 [FIX-1]");
    s_replyObject *r;

    /* 큰 값으로 SET 후 작은 값으로 갱신 반복 (heap 손상 없음 확인) */
    char big[512];
    memset(big, 'X', sizeof(big)-1); big[sizeof(big)-1]='\0';
    r=run(h,"SET","safe_k",big,NULL); CHECK(is_ok(r),"SET 큰값→OK"); reply_free(r);

    for (int i = 0; i < 50; i++) {
        char v[16]; snprintf(v,sizeof(v),"val%d",i);
        s_replyObject *s = run(h,"SET","safe_k",v,NULL);
        reply_free(s);
    }
    r=run(h,"GET","safe_k",NULL);
    CHECK(r&&r->type==REPLY_STRING,"SET 반복 갱신 후 GET 정상"); reply_free(r);

    /* heap stats: alloc/free 균형 확인 */
    HeapHeader *hh = core_heap_hdr(h);
    CHECK(hh->total_free <= hh->total_alloc, "heap free ≤ alloc (누수없음)");
}

/* ============================================================
 *  §10  멀티프로세스 stress
 * ============================================================ */
#define MP_PROCS  8
#define MP_ITER   500

static void mp_worker(const char *shm_name, int pid_idx)
{
    ShmHandle *h = shm_open_existing(shm_name);
    if (!h) exit(1);
    char k[32], v[32];
    for (int i = 0; i < MP_ITER; i++) {
        snprintf(k, sizeof(k), "mp_%d_%d", pid_idx, i);
        snprintf(v, sizeof(v), "v%d_%d",   pid_idx, i);
        string_t sk={k,(uint32_t)strlen(k)}, sv={v,(uint32_t)strlen(v)};
        string_t sc={"SET",3};
        string_t *args[]={&sc,&sk,&sv};
        s_replyObject *r = cmd_dispatch(h, args, 3);
        reply_free(r);
    }
    shm_close(h);
    exit(0);
}

static void t10_multiprocess(ShmHandle *h)
{
    SECT("10. 멀티프로세스 stress (8 proc × 500 iter)");
    pid_t pids[MP_PROCS];
    for (int i = 0; i < MP_PROCS; i++) {
        pids[i] = fork();
        if (pids[i] == 0)	{
			shm_close(h);
			mp_worker(SHM_NAME, i);
		}
    }
    for (int i = 0; i < MP_PROCS; i++) waitpid(pids[i], NULL, 0);

    ShmHandle *vh = shm_open_existing(SHM_NAME);
    int ok = 1;
    for (int i = 0; i < MP_PROCS && ok; i++) {
        for (int j = 0; j < MP_ITER && ok; j++) {
            char k[32]; snprintf(k, sizeof(k), "mp_%d_%d", i, j);
            string_t sk={k,(uint32_t)strlen(k)}, sc={"GET",3};
            string_t *a[]={&sc,&sk};
            s_replyObject *r = cmd_dispatch(vh, a, 2);
            if (!r || r->type != REPLY_STRING) ok = 0;
            reply_free(r);
        }
    }
    CHECK(ok, "멀티프로세스: 모든 키 정상 저장");
    shm_close(vh);
}

/* ============================================================ §01 */
static void t01_bset_basic(ShmHandle *h)
{
    SECT("01. 기본: BSET / BGET / BCARD");
    s_replyObject *r;

    r=run(h,"BSET","k","100","alice",NULL); CHECK(is_int(r,1),"BSET 신규→1"); reply_free(r);
    r=run(h,"BGET","k","100",NULL);         CHECK(is_str(r,"alice"),"BGET 100=alice"); reply_free(r);
    r=run(h,"BCARD","k",NULL);              CHECK(is_int(r,1),"BCARD=1"); reply_free(r);
    r=run(h,"BGET","k","999",NULL);         CHECK(is_nil(r),"BGET 없는score→NIL"); reply_free(r);
    r=run(h,"BGET","nokey","100",NULL);     CHECK(is_nil(r),"BGET 없는key→NIL"); reply_free(r);
    /* 인자 부족 */
    r=run(h,"BSET","k","100",NULL);         CHECK(is_err(r),"BSET 인자부족→ERR"); reply_free(r);
}

/* ============================================================ §02 */
static void t02_bset_multi(ShmHandle *h)
{
    SECT("02. 멀티 score/value 삽입");
    s_replyObject *r;

    r=run(h,"BSET","m","10","a","20","b","30","c",NULL);
    CHECK(is_int(r,3),"BSET 3쌍→3"); reply_free(r);
    r=run(h,"BCARD","m",NULL); CHECK(is_int(r,3),"BCARD=3"); reply_free(r);
    r=run(h,"BGET","m","20",NULL); CHECK(is_str(r,"b"),"BGET 20=b"); reply_free(r);
}

/* ============================================================ §03 */
static void t03_bset_update(ShmHandle *h)
{
    SECT("03. 동일 score 갱신 (value 교체)");
    s_replyObject *r;

    r=run(h,"BSET","m","20","B_updated",NULL);
    CHECK(is_int(r,0),"BSET 갱신→0"); reply_free(r);
    r=run(h,"BGET","m","20",NULL); CHECK(is_str(r,"B_updated"),"갱신 후 BGET=B_updated"); reply_free(r);
    r=run(h,"BCARD","m",NULL);     CHECK(is_int(r,3),"갱신 후 BCARD 변화없음=3"); reply_free(r);
}

/* ============================================================ §04 */
static void t04_bset_rank(ShmHandle *h)
{
    SECT("04. BRANK");
    s_replyObject *r;

    /* m: score 10(a) 20(B_updated) 30(c) */
    r=run(h,"BRANK","m","10",NULL); CHECK(is_int(r,0),"BRANK 10=0(최소)"); reply_free(r);
    r=run(h,"BRANK","m","20",NULL); CHECK(is_int(r,1),"BRANK 20=1");       reply_free(r);
    r=run(h,"BRANK","m","30",NULL); CHECK(is_int(r,2),"BRANK 30=2(최대)"); reply_free(r);
    r=run(h,"BRANK","m","99",NULL); CHECK(is_nil(r),"BRANK 없음→NIL");    reply_free(r);
}

/* ============================================================ §05 */
static void t05_bset_bdel(ShmHandle *h)
{
    SECT("05. BDEL");
    s_replyObject *r;

    reply_free(run(h,"BSET","d","1","x","2","y","3","z","4","w",NULL));

    r=run(h,"BDEL","d","2","3",NULL); CHECK(is_int(r,2),"BDEL 2개→2"); reply_free(r);
    r=run(h,"BCARD","d",NULL);        CHECK(is_int(r,2),"BDEL 후 BCARD=2"); reply_free(r);
    r=run(h,"BGET","d","2",NULL);     CHECK(is_nil(r),"BDEL 후 BGET 2→NIL"); reply_free(r);

    r=run(h,"BDEL","d","99",NULL);    CHECK(is_int(r,0),"BDEL 없는score→0"); reply_free(r);
    r=run(h,"BDEL","nokey","1",NULL); CHECK(is_int(r,0),"BDEL 없는key→0"); reply_free(r);
}

/* ============================================================ §06 */
static void t06_bset_brange(ShmHandle *h)
{
    SECT("06. BRANGE (정방향 / 음수 인덱스)");
    s_replyObject *r;

    /* r_key: 10,20,30,40,50 */
    reply_free(run(h,"BSET","r","10","aa","20","bb","30","cc","40","dd","50","ee",NULL));

    r=run(h,"BRANGE","r","0","4",NULL);
    CHECK(arr_sz(r,10),"BRANGE 0 4 크기=10");
    CHECK(arr_str(r,0,"10"),"BRANGE[0]=10"); CHECK(arr_str(r,1,"aa"),"BRANGE[1]=aa");
    CHECK(arr_str(r,8,"50"),"BRANGE[8]=50"); CHECK(arr_str(r,9,"ee"),"BRANGE[9]=ee");
    reply_free(r);

    /* 부분 범위 */
    r=run(h,"BRANGE","r","1","2",NULL);
    CHECK(arr_sz(r,4),"BRANGE 1 2 크기=4");
    CHECK(arr_str(r,0,"20"),"BRANGE 1 2 [0]=20"); reply_free(r);

    /* 음수 인덱스: -1=마지막 */
    r=run(h,"BRANGE","r","-1","-1",NULL);
    CHECK(arr_sz(r,2),"BRANGE -1 -1 크기=2");
    CHECK(arr_str(r,0,"50"),"BRANGE -1 [0]=50"); reply_free(r);

    /* -3~-1 = idx 2,3,4 */
    r=run(h,"BRANGE","r","-3","-1",NULL);
    CHECK(arr_sz(r,6),"BRANGE -3 -1 크기=6");
    CHECK(arr_str(r,0,"30"),"BRANGE -3 [0]=30"); reply_free(r);

    /* 범위 초과 → 빈 배열 */
    r=run(h,"BRANGE","r","10","20",NULL);
    CHECK(arr_sz(r,0),"BRANGE 초과→0"); reply_free(r);
}

/* ============================================================ §07 */
static void t07_bset_brangebyscore(ShmHandle *h)
{
    SECT("07. BRANGEBYSCORE [LIMIT]");
    s_replyObject *r;

    /* r_key: 10~50 */
    r=run(h,"BRANGEBYSCORE","r","10","50",NULL);
    CHECK(arr_sz(r,10),"BRANGEBYSCORE 10~50=10"); reply_free(r);

    r=run(h,"BRANGEBYSCORE","r","20","40",NULL);
    CHECK(arr_sz(r,6),"BRANGEBYSCORE 20~40=6");
    CHECK(arr_str(r,0,"20"),"[0]=20"); reply_free(r);

    /* LIMIT offset=1 count=2 */
    r=run(h,"BRANGEBYSCORE","r","10","50","LIMIT","1","2",NULL);
    CHECK(arr_sz(r,4),"BRANGEBYSCORE LIMIT 1 2 크기=4");
    CHECK(arr_str(r,0,"20"),"LIMIT 후 [0]=20"); reply_free(r);

    /* 범위 밖 */
    r=run(h,"BRANGEBYSCORE","r","100","200",NULL);
    CHECK(arr_sz(r,0),"BRANGEBYSCORE 범위밖=0"); reply_free(r);
}

/* ============================================================ §08 */
static void t08_bset_bcount(ShmHandle *h)
{
    SECT("08. BCOUNT");
    s_replyObject *r;

    r=run(h,"BCOUNT","r","10","50",NULL); CHECK(is_int(r,5),"BCOUNT 10~50=5"); reply_free(r);
    r=run(h,"BCOUNT","r","20","30",NULL); CHECK(is_int(r,2),"BCOUNT 20~30=2"); reply_free(r);
    r=run(h,"BCOUNT","r","15","25",NULL); CHECK(is_int(r,1),"BCOUNT 15~25=1"); reply_free(r);
    r=run(h,"BCOUNT","r","100","200",NULL); CHECK(is_int(r,0),"BCOUNT 범위밖=0"); reply_free(r);
}

/* ============================================================ §09 */
static void t09_bset_pop(ShmHandle *h)
{
    SECT("09. BPOPMIN / BPOPMAX");
    s_replyObject *r;

    reply_free(run(h,"BSET","p","1","v1","2","v2","3","v3","4","v4","5","v5",NULL));

    r=run(h,"BPOPMIN","p",NULL);
    CHECK(arr_sz(r,2),"BPOPMIN 크기=2");
    CHECK(arr_str(r,0,"1"),"BPOPMIN score=1");
    CHECK(arr_str(r,1,"v1"),"BPOPMIN val=v1"); reply_free(r);

    r=run(h,"BCARD","p",NULL); CHECK(is_int(r,4),"BPOPMIN 후 BCARD=4"); reply_free(r);

    r=run(h,"BPOPMAX","p","2",NULL);
    CHECK(arr_sz(r,4),"BPOPMAX 2 크기=4");
    CHECK(arr_str(r,0,"5"),"BPOPMAX[0] score=5");
    CHECK(arr_str(r,2,"4"),"BPOPMAX[2] score=4"); reply_free(r);

    r=run(h,"BCARD","p",NULL); CHECK(is_int(r,2),"BPOPMAX 후 BCARD=2"); reply_free(r);

    /* 남은 것 모두 pop */
    r=run(h,"BPOPMIN","p","10",NULL);
    CHECK(arr_sz(r,4),"BPOPMIN 10 (남은 2개)=4"); reply_free(r);
    r=run(h,"BCARD","p",NULL); CHECK(is_int(r,0),"전부 pop 후 BCARD=0"); reply_free(r);
}

/* ============================================================ §10 */
static void t10_bset_drop_recreate(ShmHandle *h)
{
    SECT("10. BDROP 후 재생성 / 타입 충돌");
    s_replyObject *r;

    r=run(h,"BSET","drp","1","v",NULL); reply_free(r);
    r=run(h,"BDROP","drp",NULL);
    CHECK(r&&r->type==REPLY_STATUS,"BDROP→OK"); reply_free(r);
    r=run(h,"BCARD","drp",NULL); CHECK(is_int(r,0),"BDROP 후 BCARD=0"); reply_free(r);

    r=run(h,"BDROP","nokey",NULL); CHECK(is_err(r),"BDROP 없는key→ERR"); reply_free(r);

    /* 재생성 */
    r=run(h,"BSET","drp","42","newval",NULL); CHECK(is_int(r,1),"재생성→1"); reply_free(r);
    r=run(h,"BGET","drp","42",NULL); CHECK(is_str(r,"newval"),"재생성 BGET=newval"); reply_free(r);

    /* 타입 충돌: ZADD 후 BSET */
    r=run(h,"ZADD","ztype","1","m",NULL); reply_free(r);
    r=run(h,"BSET","ztype","1","v",NULL); CHECK(is_err(r),"BSET→ZSET key ERR"); reply_free(r);

    /* 타입 충돌: BSET 후 HSET */
    r=run(h,"BSET","btype","1","v",NULL); reply_free(r);
    r=run(h,"HSET","btype","f","v",NULL); CHECK(is_err(r),"HSET→BSET key ERR"); reply_free(r);
}

/* ============================================================ §11 */
static void t11_bset_del_routing(ShmHandle *h)
{
    SECT("11. DEL 라우팅 (ENTRY_BSET → drop_bset)");
    s_replyObject *r;

    reply_free(run(h,"BSET","del_b","1","v1","2","v2","3","v3",NULL));

    r=run(h,"DEL","del_b",NULL);
    CHECK(is_int(r,1),"DEL BSET→1"); reply_free(r);

    r=run(h,"BCARD","del_b",NULL); CHECK(is_int(r,0),"DEL 후 BCARD=0"); reply_free(r);

    /* DEL 라우팅 테이블에 BSET 포함 확인 */
    size_t cnt=0; const DelRouteEntry *tbl=del_route_table_get(&cnt);
    int has_bset=0;
    for(size_t i=0;i<cnt;i++) if(tbl[i].entry_type==ENTRY_BSET) has_bset=1;
    CHECK(has_bset,"라우팅 테이블에 ENTRY_BSET 등록됨");

    /* 혼합 DEL */
    r=run(h,"SET","mv","v",NULL); reply_free(r);
    r=run(h,"BSET","mb","1","v",NULL); reply_free(r);
    r=run(h,"HSET","mh","f","v",NULL); reply_free(r);
    r=run(h,"DEL","mv","mb","mh",NULL);
    CHECK(is_int(r,3),"DEL KV+BSET+HASH 혼합→3"); reply_free(r);
}

/* ============================================================ §12 */
static void t12_bset_grow(ShmHandle *h)
{
    SECT("12. 배열 자동 확장 (BSET_CHUNK 초과 삽입)");
    s_replyObject *r;

    /* BSET_CHUNK=1024개 초과 삽입 (1200개) */
    const int N = 1200;
    for(int i=0;i<N;i++){
        char sc[16],v[16];
        snprintf(sc,sizeof(sc),"%d",i);
        snprintf(v, sizeof(v),"v%d",i);
        r=run(h,"BSET","grow_k",sc,v,NULL); reply_free(r);
    }
    r=run(h,"BCARD","grow_k",NULL);
    CHECK(is_int(r,N),"BSET_CHUNK 초과 후 BCARD=1200"); reply_free(r);

    /* capacity 확인: BSetHeader 직접 조회 */
    BSetHeader *bh = core_bset_get(h,"grow_k",6);
    CHECK(bh && bh->capacity >= (uint64_t)N,
          "capacity ≥ N (자동 확장됨)");
    CHECK(bh && bh->capacity % BSET_CHUNK == 0,
          "capacity는 BSET_CHUNK 배수");

    /* 정렬 순서 확인 (첫 5개) */
    r=run(h,"BRANGE","grow_k","0","4",NULL);
    CHECK(arr_sz(r,10),"BRANGE 0 4 크기=10");
    CHECK(arr_str(r,0,"0"),"grow 후 BRANGE[0]=score 0");
    reply_free(r);

    r=run(h,"BDROP","grow_k",NULL);
    reply_free(r);
}

/* ============================================================ §13 */
static void t13_bset_shrink(ShmHandle *h)
{
    SECT("13. 배열 자동 축소 (대량 삭제 후 shrink)");

    /* 2048개 삽입 (capacity=2×BSET_CHUNK) */
    const int N = (int)(2 * BSET_CHUNK);
    for(int i=0;i<N;i++){
        char sc[16],v[16];
        snprintf(sc,sizeof(sc),"%d",i+10000); /* score 충돌 방지 */
        snprintf(v, sizeof(v),"v%d",i);
        s_replyObject *r=run(h,"BSET","shrk_k",sc,v,NULL); reply_free(r);
    }
    BSetHeader *bh = core_bset_get(h,"shrk_k",6);
    CHECK(bh && bh->capacity >= (uint64_t)N, "삽입 후 capacity ≥ N");

    /* 앞쪽 75% 삭제 → count < capacity/2 → shrink 발동 */
    int del_cnt = (int)(N * 3 / 4);
    for(int i=0;i<del_cnt;i++){
        char sc[16]; snprintf(sc,sizeof(sc),"%d",i+10000);
        s_replyObject *r=run(h,"BDEL","shrk_k",sc,NULL); reply_free(r);
    }
    /* shrink 확인 */
    CHECK(bh->count == (uint64_t)(N - del_cnt), "삭제 후 count 정확");
    CHECK(bh->capacity < (uint64_t)N, "shrink 후 capacity 감소");
    CHECK(bh->capacity >= BSET_CHUNK,  "shrink 후 capacity ≥ BSET_CHUNK");

    s_replyObject *r=run(h,"BDROP","shrk_k",NULL);
	reply_free(r);
}

/* ============================================================ §14 */
#define MT_THREADS  8
#define MT_ITER     500

static void *mt_bset_worker(void *arg)
{
    MtArg *a = (MtArg*)arg;
    for(int i=0;i<MT_ITER;i++){
        /* 각 스레드는 고유 score 대역 사용 */
        uint64_t score = (uint64_t)(a->id * MT_ITER + i);
        char     v[32]; snprintf(v,sizeof(v),"t%d_%d",a->id,i);
        char     sc[32]; snprintf(sc,sizeof(sc),"%llu",(unsigned long long)score);

        string_t a0={"BSET",4}, a1={"mt_key",6};
        string_t a2={sc,(uint32_t)strlen(sc)};
        string_t a3={v, (uint32_t)strlen(v)};
        string_t *args[]={&a0,&a1,&a2,&a3};
        s_replyObject *r = cmd_dispatch(a->h, args, 4);
        reply_free(r);
    }
    return NULL;
}

static void t14_bset_multithread(ShmHandle *h)
{
    SECT("14. 멀티스레드 동시 BSET (serialize 검증)");

    pthread_t tids[MT_THREADS];
    MtArg     args[MT_THREADS];
    for(int i=0;i<MT_THREADS;i++){
        args[i].h=h; args[i].id=i;
        pthread_create(&tids[i],NULL,mt_bset_worker,&args[i]);
    }
    for(int i=0;i<MT_THREADS;i++) pthread_join(tids[i],NULL);

    s_replyObject *r = run(h,"BCARD","mt_key",NULL);
    CHECK(is_int(r,(int64_t)(MT_THREADS*MT_ITER)),
          "멀티스레드 BSET 후 BCARD=T×ITER"); reply_free(r);

    /* 정렬 순서 확인: 전체 BRANGE 0 9 */
    r=run(h,"BRANGE","mt_key","0","9",NULL);
    if(r&&r->type==REPLY_ARRAY&&r->elements==20){
        uint64_t prev_sc=0; int sorted=1;
        for(size_t i=0;i<20;i+=2){
            uint64_t sc=(uint64_t)strtoull((char*)r->element[i]->ptr,NULL,10);
            if(sc<prev_sc) sorted=0;
            prev_sc=sc;
        }
        CHECK(sorted,"BRANGE 결과 오름차순 정렬");
    } else {
        CHECK(0,"BRANGE 0 9 응답 이상");
    }
    reply_free(r);

    r=run(h,"BDROP","mt_key",NULL);
	reply_free(r);
}

/* ============================================================ §15 */
static void mp_bset_proc(const char *shm_name, int id)
{
    ShmHandle *h=shm_open_existing(shm_name);
    if(!h) exit(1);
    for(int i=0;i<MP_ITER;i++){
        uint64_t score=(uint64_t)(id*MP_ITER+i);
        char v[32],sc[32];
        snprintf(v,sizeof(v),"p%d_%d",id,i);
        snprintf(sc,sizeof(sc),"%llu",(unsigned long long)score);
        string_t a0={"BSET",4},a1={"mp_key",6};
        string_t a2={sc,(uint32_t)strlen(sc)};
        string_t a3={v,(uint32_t)strlen(v)};
        string_t *args[]={&a0,&a1,&a2,&a3};
        s_replyObject *r=cmd_dispatch(h,args,4); reply_free(r);
    }
    shm_close(h);
    exit(0);
}

static void t15_bset_multiprocess(ShmHandle *h)
{
    SECT("15. 멀티프로세스 동시 BSET (8 proc × 200 iter)");
    (void)h;

    pid_t pids[MP_PROCS];
    for(int i=0;i<MP_PROCS;i++){
        pids[i]=fork();
        if(pids[i]==0) {
			shm_close(h);
			mp_bset_proc(SHM_NAME,i);
		}
    }
    for(int i=0;i<MP_PROCS;i++) waitpid(pids[i],NULL,0);

    ShmHandle *vh=shm_open_existing(SHM_NAME);
    s_replyObject *r=run(vh,"BCARD","mp_key",NULL);
    CHECK(is_int(r,(int64_t)(MP_PROCS*MP_ITER)),
          "멀티프로세스 BSET 후 BCARD=P×ITER"); reply_free(r);

    /* 임의 score 존재 확인 */
    char sc[32]; snprintf(sc,sizeof(sc),"%d", (MP_PROCS/2)*MP_ITER + MP_ITER/2);
    r=run(vh,"BGET","mp_key",sc,NULL);
    CHECK(r&&r->type==REPLY_STRING,"멀티프로세스: 임의 score BGET 성공"); reply_free(r);

    r=run(vh,"BDROP","mp_key",NULL);
	reply_free(r);
    shm_close(vh);
}

#if 0
/* ============================================================
 *  §16  CSET (Chunk-based Sorted Set)
 * ============================================================ */
static void t16_cset_basic(ShmHandle *h)
{
    SECT("16. CSET 기본: CSET / CGET / CCARD");
    s_replyObject *r;

    r=run(h,"CSET","c1","100","alice",NULL); 
    CHECK(is_int(r,1),"CSET 신규→1"); reply_free(r);

    r=run(h,"CGET","c1","100",NULL); 
    CHECK(arr_sz(r,2) && arr_str(r,1,"alice"),"CGET [score,value]"); reply_free(r);

    r=run(h,"CCARD","c1",NULL); 
    CHECK(is_int(r,1),"CCARD=1"); reply_free(r);
}

static void t17_cset_multi(ShmHandle *h)
{
    SECT("17. CSET 멀티 삽입 + 동일 score 갱신");
    s_replyObject *r;

    r=run(h,"CSET","c2","10","a","20","b","30","c",NULL);
    CHECK(is_int(r,3),"CSET 3쌍→3"); reply_free(r);

    /* 동일 score value 갱신 */
    r=run(h,"CSET","c2","20","b_updated",NULL);
    CHECK(is_int(r,0),"동일 score 갱신→0"); reply_free(r);

    r=run(h,"CGET","c2","20",NULL);
    CHECK(arr_str(r,1,"b_updated"),"value 갱신 확인"); reply_free(r);
}

static void t18_cset_rank_range(ShmHandle *h)
{
    SECT("18. CRANK / CRANGE / CRANGEBYSCORE");
    s_replyObject *r;

    /* c3: 10(a), 20(b), 30(c) */
    run(h,"CSET","c3","10","a","20","b","30","c",NULL);

    r=run(h,"CRANK","c3","20",NULL); CHECK(is_int(r,1),"CRANK b=1"); reply_free(r);
    r=run(h,"CRANK","c3","99",NULL); CHECK(is_nil(r),"CRANK 없음→NIL"); reply_free(r);

    r=run(h,"CRANGE","c3","0","2",NULL);
    CHECK(arr_sz(r,9),"CRANGE 0 2 = 3*3"); reply_free(r);

    r=run(h,"CRANGEBYSCORE","c3","15","25",NULL);
    CHECK(arr_sz(r,3),"CRANGEBYSCORE 15~25=1"); reply_free(r);
}

static void t19_cset_pop_del(ShmHandle *h)
{
    SECT("19. CPOPMIN / CDEL / CDROP");
    s_replyObject *r;

    run(h,"CSET","cp","5","x","10","y","15","z",NULL);

    r=run(h,"CPOPMIN","cp",NULL);
    CHECK(arr_sz(r,2) && arr_str(r,0,"5"),"CPOPMIN x"); reply_free(r);

    r=run(h,"CDEL","cp","15",NULL);
    CHECK(is_int(r,1),"CDEL 1"); reply_free(r);

    r=run(h,"CCARD","cp",NULL); CHECK(is_int(r,1),"CCARD=1"); reply_free(r);

    r=run(h,"CDROP","cp",NULL); CHECK(is_ok(r),"CDROP→OK"); reply_free(r);
}

static void t20_cset_multithread(ShmHandle *h)
{
    SECT("20. CSET 멀티스레드 (serialize 검증)");
    pthread_t tids[MT_THREADS];
    MtArg args[MT_THREADS];

    for(int i=0; i<MT_THREADS; i++){
        args[i].h = h; args[i].id = i;
        pthread_create(&tids[i], NULL, mt_bset_worker, &args[i]);  /* BSET worker 재사용 가능 */
    }
    for(int i=0; i<MT_THREADS; i++) pthread_join(tids[i], NULL);

    s_replyObject *r = run(h,"CCARD","mt_key",NULL);
    CHECK(is_int(r, (int64_t)(MT_THREADS*MT_ITER)), "CSET 멀티스레드 카운트 정확");
    reply_free(r);
}
#endif


/* ============================================================
 *  main
 * ============================================================ */
int main(void)
{
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║       mredis 전체 통합 테스트                ║\n");
    printf("╚══════════════════════════════════════════════╝\n");

    shm_destroy(SHM_NAME);
    ShmHandle *h = shm_create(SHM_NAME, 256ULL * 1024 * 1024);
    if (!h) { fprintf(stderr, "SHM 생성 실패\n"); return 1; }
    shm_set_debug_level(DBG_ERROR);

#if 0
    t01_bset_basic(h);
	shm_close(h);
	return 0;
#endif

    t01_kv(h);
    t02_keys(h);
    t03_del(h);
    t04_zset(h);
    t05_hash(h);
    t06_serialize(h);
    t07_zincrby_toctou(h);
    t08_hlen_mutex(h);
    t09_set_update_safety(h);
    t10_multiprocess(h);

   t01_bset_basic(h);     t02_bset_multi(h);    t03_bset_update(h);
   t04_bset_rank(h);      t05_bset_bdel(h);     t06_bset_brange(h);
   t07_bset_brangebyscore(h);t08_bset_bcount(h);t09_bset_pop(h);
   t10_bset_drop_recreate(h);t11_bset_del_routing(h);
   t12_bset_grow(h);      t13_bset_shrink(h);
   t14_bset_multithread(h);t15_bset_multiprocess(h);

#if 0
    t16_cset_basic(h);
    t17_cset_multi(h);
    t18_cset_rank_range(h);
    t19_cset_pop_del(h);
    t20_cset_multithread(h);
#endif

	s_replyObject *r = run(h,"KEYS","*",NULL);
	for (size_t i=0;i<r->elements;i++)	{
		char	tmpstr[1024];
		sprintf (tmpstr, "%.*s", (int)r->element[i]->len, (char*)r->element[i]->ptr);
		reply_free (run(h,"DEL", tmpstr, NULL));
	}
	reply_free(r);
	r = run(h,"KEYS","*",NULL);
	reply_print(r, 0);
	reply_free(r);
	shm_dump_stats(h);
    shm_close(h);
    shm_destroy(SHM_NAME);

    printf("\n══════════════════════════════════════════════\n");
    printf("결과: \033[32mPASS %d\033[0m / \033[31mFAIL %d\033[0m / 합계 %d\n",
           g_pass, g_fail, g_pass + g_fail);
    printf("══════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
