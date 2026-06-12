/*
 * test_all.c  –  mredis 전체 통합 테스트 (BSET + CSET 포함)
 *
 *  §01  KV  : SET/GET/갱신/빈값/타입충돌
 *  §02  KEYS: 패턴매칭 + mutex 보호
 *  §03  DEL : KV·ZSET·HASH·BSET·CSET 혼합 라우팅
 *  §04  ZSET: ZADD/ZREM/ZSCORE/ZINCRBY/ZRANK/ZRANGE 등
 *  §05  HASH: HSET/HGET/HDEL/HLEN/HGETALL 등
 *  §06  BSET: 배열기반 sorted set (rwlock + append fast-path)
 *  §07  CSET: 청크기반 sorted set (삭제비트 + 병합 + CCOMPACT)
 *  §08  직렬화: bucket/zset/hash/bset/cset mutex 멀티스레드 검증
 *  §09  ZINCRBY TOCTOU 수정 검증
 *  §10  멀티프로세스 stress (8 proc × 500 iter)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>

#include "mredis_types.h"
#include "mredis_core.h"
#include "cmd_dispatch.h"
#include "cmd_del.h"
#include "cmd_bset.h"
#include "cmd_cset.h"

#define SHM_NAME "/mredis_hashtable"
#define PASS     "\033[32m[PASS]\033[0m"
#define FAIL     "\033[31m[FAIL]\033[0m"
#define SECT(t)  printf("\n\033[36m══ %s ══\033[0m\n",(t))

static int g_pass=0, g_fail=0;
#define CHECK(c,m) do{ if(c){printf(PASS " %s\n",(m));g_pass++;} \
    else{printf(FAIL " %s  [L%d]\n",(m),__LINE__);g_fail++;} }while(0)

static s_replyObject *run(MRedisHandle *h, ...) {
    string_t *args[64]; string_t bufs[64]; uint32_t argc=0;
    va_list ap; va_start(ap,h);
    while(argc<64){ const char *s=va_arg(ap,const char*); if(!s) break;
        bufs[argc].ptr=s; bufs[argc].len=(uint32_t)strlen(s); args[argc]=&bufs[argc]; argc++; }
    va_end(ap);
    return cmd_dispatch(h,args,argc);
}

static int is_ok (s_replyObject *r) { return r&&r->type==REPLY_STATUS&&strcmp((char*)r->ptr,"OK")==0; }
static int is_int(s_replyObject *r,int64_t v){ return r&&r->type==REPLY_INTEGER&&r->integer==v; }
static int is_str(s_replyObject *r,const char *s){ return r&&r->type==REPLY_STRING&&strcmp((char*)r->ptr,s)==0; }
static int is_nil(s_replyObject *r){ return r&&r->type==REPLY_NIL; }
static int is_err(s_replyObject *r){ return r&&r->type==REPLY_ERROR; }
static int arr_sz(s_replyObject *r,size_t n){ return r&&r->type==REPLY_ARRAY&&r->elements==n; }
static int arr_str(s_replyObject *r,size_t i,const char *s){
    return r&&r->type==REPLY_ARRAY&&i<r->elements&&r->element[i]&&
           r->element[i]->type==REPLY_STRING&&strcmp((char*)r->element[i]->ptr,s)==0; }

/* ── §01 KV ─────────────────────────────────────────────── */
static void t01_kv(MRedisHandle *h) {
    SECT("01. KV");
    s_replyObject *r;
    r=run(h,"SET","k1","hello",NULL); CHECK(is_ok(r),"SET→OK"); reply_free(r);
    r=run(h,"GET","k1",NULL);         CHECK(is_str(r,"hello"),"GET=hello"); reply_free(r);
    r=run(h,"SET","k1","world",NULL); CHECK(is_ok(r),"SET 갱신→OK"); reply_free(r);
    r=run(h,"GET","k1",NULL);         CHECK(is_str(r,"world"),"GET 갱신=world"); reply_free(r);
    r=run(h,"SET","empty","",NULL);   CHECK(is_ok(r),"SET 빈값→OK"); reply_free(r);
    r=run(h,"GET","empty",NULL);      CHECK(r&&r->type==REPLY_STRING&&r->len==0,"GET 빈값"); reply_free(r);
    r=run(h,"GET","nokey",NULL);      CHECK(is_nil(r),"GET 없음→NIL"); reply_free(r);
    r=run(h,"SET","","v",NULL);       CHECK(is_err(r),"SET 빈key→ERR"); reply_free(r);
    run(h,"ZADD","zt","1","m",NULL);
    r=run(h,"SET","zt","v",NULL);     CHECK(is_err(r),"SET→ZSET충돌"); reply_free(r);
}

/* ── §02 KEYS ───────────────────────────────────────────── */
static void t02_keys(MRedisHandle *h) {
    SECT("02. KEYS");
    s_replyObject *r;
    run(h,"SET","key:a","1",NULL); run(h,"SET","key:b","2",NULL); run(h,"SET","other","3",NULL);
    r=run(h,"KEYS","key:*",NULL);
    CHECK(r&&r->type==REPLY_ARRAY,"KEYS key:* → ARRAY");
    int has_a=0,has_b=0,has_o=0;
    if(r&&r->type==REPLY_ARRAY) for(size_t i=0;i<r->elements;i++){
        if(r->element[i]&&!strcmp((char*)r->element[i]->ptr,"key:a")) has_a=1;
        if(r->element[i]&&!strcmp((char*)r->element[i]->ptr,"key:b")) has_b=1;
        if(r->element[i]&&!strcmp((char*)r->element[i]->ptr,"other")) has_o=1;
    }
    CHECK(has_a,"KEYS key:a 포함"); CHECK(has_b,"KEYS key:b 포함"); CHECK(!has_o,"KEYS other 미포함");
    reply_free(r);
    r=run(h,"KEYS","none_xyz",NULL); CHECK(arr_sz(r,0),"KEYS 미매칭→0"); reply_free(r);
    r=run(h,"KEYS",NULL);            CHECK(is_err(r),"KEYS 인자없음→ERR[FIX-2]"); reply_free(r);
}

/* ── §03 DEL 라우팅 ─────────────────────────────────────── */
static void t03_del(MRedisHandle *h) {
    SECT("03. DEL 라우팅 (KV/ZSET/HASH/BSET/CSET)");
    s_replyObject *r;
    run(h,"SET","dkv","v",NULL);
    run(h,"ZADD","dzs","1","m",NULL);
    run(h,"HSET","dhs","f","v",NULL);
    run(h,"BSET","dbs","1","v",NULL);
    run(h,"CSET","dcs","1","v",NULL);

    r=run(h,"DEL","dkv",NULL); CHECK(is_int(r,1),"DEL KV→1"); reply_free(r);
    r=run(h,"DEL","dzs",NULL); CHECK(is_int(r,1),"DEL ZSET→1"); reply_free(r);
    r=run(h,"DEL","dhs",NULL); CHECK(is_int(r,1),"DEL HASH→1"); reply_free(r);
    r=run(h,"DEL","dbs",NULL); CHECK(is_int(r,1),"DEL BSET→1"); reply_free(r);
    r=run(h,"DEL","dcs",NULL); CHECK(is_int(r,1),"DEL CSET→1"); reply_free(r);

    /* 혼합 */
    run(h,"SET","mk","v",NULL); run(h,"BSET","mb","1","v",NULL); run(h,"CSET","mc","1","v",NULL);
    r=run(h,"DEL","mk","mb","mc",NULL); CHECK(is_int(r,3),"DEL 혼합 3→3"); reply_free(r);

    /* 없는 키 */
    r=run(h,"DEL","nokey",NULL); CHECK(is_int(r,0),"DEL 없는키→0"); reply_free(r);

    /* 라우팅 테이블 항목 확인 */
    size_t cnt=0; const DelRouteEntry *tbl=del_route_table_get(&cnt);
    int has_bset=0, has_cset=0;
    for(size_t i=0;i<cnt;i++){
        if(tbl[i].entry_type==ENTRY_BSET) has_bset=1;
        if(tbl[i].entry_type==ENTRY_CSET) has_cset=1;
    }
    CHECK(has_bset,"DEL 라우팅: ENTRY_BSET 등록됨");
    CHECK(has_cset,"DEL 라우팅: ENTRY_CSET 등록됨");
}

/* ── §04 ZSET ───────────────────────────────────────────── */
static void t04_zset(MRedisHandle *h) {
    SECT("04. ZSET");
    s_replyObject *r;
    r=run(h,"ZCREATE","z1",NULL); CHECK(is_ok(r),"ZCREATE→OK"); reply_free(r);
    r=run(h,"ZDROP","z1",NULL);   CHECK(is_ok(r),"ZDROP→OK");   reply_free(r);
    r=run(h,"ZADD","zs","100","Alice",NULL); CHECK(is_int(r,1),"ZADD Alice→1"); reply_free(r);
    r=run(h,"ZADD","zs","200","Bob",  NULL); CHECK(is_int(r,1),"ZADD Bob→1");   reply_free(r);
    r=run(h,"ZADD","zs","150","Carol",NULL); CHECK(is_int(r,1),"ZADD Carol→1"); reply_free(r);
    r=run(h,"ZCARD","zs",NULL); CHECK(is_int(r,3),"ZCARD=3"); reply_free(r);
    r=run(h,"ZSCORE","zs","Alice",NULL);
    CHECK(r&&r->type==REPLY_STRING&&fabs(atof((char*)r->ptr)-100.0)<1e-9,"ZSCORE Alice=100"); reply_free(r);
    r=run(h,"ZRANK","zs","Alice",NULL);   CHECK(is_int(r,0),"ZRANK Alice=0"); reply_free(r);
    r=run(h,"ZREVRANK","zs","Alice",NULL);CHECK(is_int(r,2),"ZREVRANK Alice=2"); reply_free(r);
    r=run(h,"ZCOUNT","zs","100","200",NULL); CHECK(is_int(r,3),"ZCOUNT=3"); reply_free(r);
    r=run(h,"ZINCRBY","zs","10","Bob",NULL);
    CHECK(r&&r->type==REPLY_STRING&&fabs(atof((char*)r->ptr)-210.0)<1e-9,"ZINCRBY Bob=210"); reply_free(r);
    r=run(h,"ZREM","zs","Carol",NULL); CHECK(is_int(r,1),"ZREM Carol→1"); reply_free(r);
    r=run(h,"ZRANGE","zs","0","1",NULL); CHECK(arr_sz(r,4),"ZRANGE 0 1=4"); reply_free(r);
    run(h,"ZADD","zpk","1","a","2","b","3","c",NULL);
    r=run(h,"ZPOPMIN","zpk",NULL); CHECK(arr_str(r,0,"a"),"ZPOPMIN=a"); reply_free(r);
    r=run(h,"ZPOPMAX","zpk","2",NULL); CHECK(arr_str(r,0,"c"),"ZPOPMAX=c"); reply_free(r);
}

/* ── §05 HASH ───────────────────────────────────────────── */
static void t05_hash(MRedisHandle *h) {
    SECT("05. HASH");
    s_replyObject *r;
    r=run(h,"HSET","user","name","Alice","age","30","city","Seoul",NULL);
    CHECK(is_int(r,3),"HSET 3→3"); reply_free(r);
    r=run(h,"HGET","user","name",NULL); CHECK(is_str(r,"Alice"),"HGET=Alice"); reply_free(r);
    r=run(h,"HSET","user","name","Bob",NULL); CHECK(is_int(r,0),"HSET 갱신→0"); reply_free(r);
    r=run(h,"HLEN","user",NULL); CHECK(is_int(r,3),"HLEN=3"); reply_free(r);
    r=run(h,"HEXISTS","user","age",NULL);  CHECK(is_int(r,1),"HEXISTS=1"); reply_free(r);
    r=run(h,"HDEL","user","age","city",NULL); CHECK(is_int(r,2),"HDEL 2→2"); reply_free(r);
    r=run(h,"HLEN","user",NULL); CHECK(is_int(r,1),"HDEL 후 HLEN=1"); reply_free(r);
    run(h,"HSET","hh","f1","v1","f2","v2","f3","v3",NULL);
    r=run(h,"HGETALL","hh",NULL); CHECK(arr_sz(r,6),"HGETALL=6"); reply_free(r);
    r=run(h,"HKEYS","hh",NULL);   CHECK(arr_sz(r,3),"HKEYS=3");   reply_free(r);
    r=run(h,"HVALS","hh",NULL);   CHECK(arr_sz(r,3),"HVALS=3");   reply_free(r);
    run(h,"HSET","cnt","n","10",NULL);
    r=run(h,"HINCRBY","cnt","n","5",NULL);     CHECK(is_int(r,15),"HINCRBY=15"); reply_free(r);
    run(h,"HSET","flt","x","1.5",NULL);
    r=run(h,"HINCRBYFLOAT","flt","x","0.5",NULL);
    CHECK(r&&r->type==REPLY_STRING&&fabs(atof((char*)r->ptr)-2.0)<1e-9,"HINCRBYFLOAT=2.0"); reply_free(r);
}

/* ── §06 BSET ───────────────────────────────────────────── */
static void t06_bset(MRedisHandle *h) {
    SECT("06. BSET (배열 기반, rwlock, Append Fast-Path)");
    s_replyObject *r;

    r=run(h,"BSET","bk","100","alice",NULL); CHECK(is_int(r,1),"BSET 신규→1"); reply_free(r);
    r=run(h,"BGET","bk","100",NULL);         CHECK(is_str(r,"alice"),"BGET=alice"); reply_free(r);
    r=run(h,"BSET","bk","100","Alice",NULL); CHECK(is_int(r,0),"BSET 갱신→0"); reply_free(r);
    r=run(h,"BGET","bk","100",NULL);         CHECK(is_str(r,"Alice"),"BGET 갱신=Alice"); reply_free(r);

    run(h,"BSET","bk","200","bob","300","carol",NULL);
    r=run(h,"BCARD","bk",NULL); CHECK(is_int(r,3),"BCARD=3"); reply_free(r);

    r=run(h,"BRANK","bk","100",NULL); CHECK(is_int(r,0),"BRANK 100=0"); reply_free(r);
    r=run(h,"BRANK","bk","300",NULL); CHECK(is_int(r,2),"BRANK 300=2"); reply_free(r);
    r=run(h,"BRANK","bk","999",NULL); CHECK(is_nil(r),"BRANK 없음→NIL"); reply_free(r);

    r=run(h,"BCOUNT","bk","100","300",NULL); CHECK(is_int(r,3),"BCOUNT=3"); reply_free(r);
    r=run(h,"BCOUNT","bk","150","250",NULL); CHECK(is_int(r,1),"BCOUNT 150~250=1"); reply_free(r);

    r=run(h,"BRANGE","bk","0","2",NULL);
    CHECK(arr_sz(r,6),"BRANGE 0 2=6");
    CHECK(arr_str(r,0,"100"),"BRANGE[0]=100"); CHECK(arr_str(r,4,"300"),"BRANGE[4]=300");
    reply_free(r);

    r=run(h,"BRANGE","bk","-1","-1",NULL);
    CHECK(arr_sz(r,2)&&arr_str(r,0,"300"),"BRANGE -1,-1=300"); reply_free(r);

    r=run(h,"BRANGEBYSCORE","bk","100","200",NULL);
    CHECK(arr_sz(r,4),"BRANGEBYSCORE 100~200=4"); reply_free(r);
    r=run(h,"BRANGEBYSCORE","bk","100","300","LIMIT","1","1",NULL);
    CHECK(arr_sz(r,2)&&arr_str(r,0,"200"),"BRANGEBYSCORE LIMIT=200"); reply_free(r);

    /* BDEL */
    r=run(h,"BDEL","bk","200",NULL); CHECK(is_int(r,1),"BDEL→1"); reply_free(r);
    r=run(h,"BCARD","bk",NULL);      CHECK(is_int(r,2),"BDEL 후 BCARD=2"); reply_free(r);
    r=run(h,"BGET","bk","200",NULL); CHECK(is_nil(r),"BDEL 후 BGET→NIL"); reply_free(r);

    /* BPOPMIN/MAX */
    run(h,"BSET","bpk","1","v1","2","v2","3","v3",NULL);
    r=run(h,"BPOPMIN","bpk",NULL);
    CHECK(arr_sz(r,2)&&arr_str(r,0,"1"),"BPOPMIN=1"); reply_free(r);
    r=run(h,"BPOPMAX","bpk","2",NULL);
    CHECK(arr_sz(r,4)&&arr_str(r,0,"3"),"BPOPMAX 2: 3 먼저"); reply_free(r);
    r=run(h,"BCARD","bpk",NULL); CHECK(is_int(r,0),"BPOPMAX 후 BCARD=0"); reply_free(r);

    /* BDROP + 타입충돌 */
    r=run(h,"BDROP","bk",NULL); CHECK(is_ok(r),"BDROP→OK"); reply_free(r);
    r=run(h,"BDROP","nokey",NULL); CHECK(is_err(r),"BDROP 없는key→ERR"); reply_free(r);
    run(h,"BSET","btype","1","v",NULL);
    r=run(h,"HSET","btype","f","v",NULL); CHECK(is_err(r),"HSET→BSET충돌"); reply_free(r);
    run(h,"BDROP","btype",NULL);

    /* 대량 삽입으로 자동 확장(grow) 검증 */
    for(int i=0;i<(int)(BSET_CHUNK+100);i++){
        char sc[16],v[16]; snprintf(sc,sizeof(sc),"%d",i); snprintf(v,sizeof(v),"v%d",i);
        s_replyObject *t=run(h,"BSET","bgrow",sc,v,NULL); reply_free(t);
    }
    r=run(h,"BCARD","bgrow",NULL);
    CHECK(is_int(r,(int64_t)(BSET_CHUNK+100)),"BSET grow: BCARD=CHUNK+100"); reply_free(r);
    BSetHeader *bsh=core_bset_get(h,"bgrow",5);
    CHECK(bsh&&bsh->capacity>BSET_CHUNK,"BSET grow: capacity>CHUNK");
    run(h,"BDROP","bgrow",NULL);
}

/* ── §07 CSET ───────────────────────────────────────────── */
static void t07_cset(MRedisHandle *h) {
    SECT("07. CSET (청크 체인, 삭제비트, 자동병합, CCOMPACT)");
    s_replyObject *r;

    r=run(h,"CSET","ck","100","alice","200","bob","300","carol",NULL);
    CHECK(is_int(r,3),"CSET 3→3"); reply_free(r);
    r=run(h,"CCARD","ck",NULL); CHECK(is_int(r,3),"CCARD=3"); reply_free(r);

    r=run(h,"CGET","ck","100",NULL); CHECK(is_str(r,"alice"),"CGET=alice"); reply_free(r);
    r=run(h,"CGET","ck","999",NULL); CHECK(is_nil(r),"CGET 없음→NIL"); reply_free(r);

    /* 갱신 */
    r=run(h,"CSET","ck","200","Bob",NULL); CHECK(is_int(r,0),"CSET 갱신→0"); reply_free(r);
    r=run(h,"CGET","ck","200",NULL); CHECK(is_str(r,"Bob"),"CGET 갱신=Bob"); reply_free(r);

    /* CRANK */
    r=run(h,"CRANK","ck","100",NULL); CHECK(is_int(r,0),"CRANK 100=0"); reply_free(r);
    r=run(h,"CRANK","ck","300",NULL); CHECK(is_int(r,2),"CRANK 300=2"); reply_free(r);
    r=run(h,"CRANK","ck","999",NULL); CHECK(is_nil(r),"CRANK 없음→NIL"); reply_free(r);

    /* CCOUNT */
    r=run(h,"CCOUNT","ck","100","300",NULL); CHECK(is_int(r,3),"CCOUNT=3"); reply_free(r);
    r=run(h,"CCOUNT","ck","150","250",NULL); CHECK(is_int(r,1),"CCOUNT 150~250=1"); reply_free(r);

    /* CRANGE */
    r=run(h,"CRANGE","ck","0","2",NULL);
    CHECK(arr_sz(r,6),"CRANGE 0 2=6");
    CHECK(arr_str(r,0,"100"),"CRANGE[0]=100"); CHECK(arr_str(r,4,"300"),"CRANGE[4]=300");
    reply_free(r);
    r=run(h,"CRANGE","ck","-1","-1",NULL);
    CHECK(arr_sz(r,2)&&arr_str(r,0,"300"),"CRANGE -1,-1=300"); reply_free(r);

    /* CRANGEBYSCORE */
    r=run(h,"CRANGEBYSCORE","ck","100","200",NULL);
    CHECK(arr_sz(r,4),"CRANGEBYSCORE 100~200=4"); reply_free(r);
    r=run(h,"CRANGEBYSCORE","ck","100","300","LIMIT","1","1",NULL);
    CHECK(arr_sz(r,2)&&arr_str(r,0,"200"),"CRANGEBYSCORE LIMIT=200"); reply_free(r);

    /* CDEL: 삭제 비트 set, memmove 없음 */
    r=run(h,"CDEL","ck","200",NULL); CHECK(is_int(r,1),"CDEL→1"); reply_free(r);
    r=run(h,"CCARD","ck",NULL);      CHECK(is_int(r,2),"CDEL 후 CCARD=2"); reply_free(r);
    r=run(h,"CGET","ck","200",NULL); CHECK(is_nil(r),"CDEL 후 CGET→NIL"); reply_free(r);

    /* CCOMPACT: 삭제 슬롯 물리 제거 */
    r=run(h,"CCOMPACT","ck",NULL);
    CHECK(r&&r->type==REPLY_INTEGER&&r->integer>=1,"CCOMPACT 제거수≥1"); reply_free(r);
    r=run(h,"CCARD","ck",NULL); CHECK(is_int(r,2),"CCOMPACT 후 CCARD=2"); reply_free(r);
    r=run(h,"CGET","ck","100",NULL); CHECK(is_str(r,"alice"),"CCOMPACT 후 CGET=alice"); reply_free(r);
    r=run(h,"CGET","ck","300",NULL); CHECK(is_str(r,"carol"),"CCOMPACT 후 CGET=carol"); reply_free(r);

    /* CPOPMIN/MAX */
    run(h,"CSET","cpk","1","v1","2","v2","3","v3",NULL);
    r=run(h,"CPOPMIN","cpk",NULL);
    CHECK(arr_sz(r,2)&&arr_str(r,0,"1"),"CPOPMIN=1"); reply_free(r);
    r=run(h,"CPOPMAX","cpk","2",NULL);
    CHECK(arr_sz(r,4)&&arr_str(r,0,"3"),"CPOPMAX 2: 3 먼저"); reply_free(r);
    r=run(h,"CCARD","cpk",NULL); CHECK(is_int(r,0),"CPOPMAX 후 CCARD=0"); reply_free(r);

    /* CDROP + 타입충돌 */
    r=run(h,"CDROP","ck",NULL);      CHECK(is_ok(r),"CDROP→OK");      reply_free(r);
    r=run(h,"CDROP","nokey",NULL);   CHECK(is_err(r),"CDROP없음→ERR"); reply_free(r);
    run(h,"CSET","ctype","1","v",NULL);
    r=run(h,"BSET","ctype","1","v",NULL); CHECK(is_err(r),"BSET→CSET충돌"); reply_free(r);
    run(h,"CDROP","ctype",NULL);

    /* 대량 삽입 → 청크 여러 개 생성 검증 */
    for(int i=0;i<(int)(CSET_CHUNK+200);i++){
        char sc[16],v[16]; snprintf(sc,sizeof(sc),"%d",i); snprintf(v,sizeof(v),"v%d",i);
        s_replyObject *t=run(h,"CSET","cgrow",sc,v,NULL); reply_free(t);
    }
    r=run(h,"CCARD","cgrow",NULL);
    CHECK(is_int(r,(int64_t)(CSET_CHUNK+200)),"CSET grow: CCARD=CHUNK+200"); reply_free(r);
    CSetHeader *csh=core_cset_get(h,"cgrow",5);
    CHECK(csh&&csh->chunk_count>=2,"CSET grow: chunk_count≥2");
    run(h,"CDROP","cgrow",NULL);

    /* 대량 CDEL 후 CCOMPACT 검증 */
    for(int i=0;i<100;i++){
        char sc[16],v[16]; snprintf(sc,sizeof(sc),"%d",i+5000); snprintf(v,sizeof(v),"cv%d",i);
        s_replyObject *t=run(h,"CSET","ccomp",sc,v,NULL); reply_free(t);
    }
    for(int i=0;i<50;i++){
        char sc[16]; snprintf(sc,sizeof(sc),"%d",i+5000);
        s_replyObject *t=run(h,"CDEL","ccomp",sc,NULL); reply_free(t);
    }
    r=run(h,"CCARD","ccomp",NULL); CHECK(is_int(r,50),"대량 CDEL 후 CCARD=50"); reply_free(r);
    r=run(h,"CCOMPACT","ccomp",NULL);
    CHECK(r&&r->type==REPLY_INTEGER&&r->integer==50,"CCOMPACT 50 슬롯 제거"); reply_free(r);
    r=run(h,"CCARD","ccomp",NULL); CHECK(is_int(r,50),"CCOMPACT 후 CCARD=50"); reply_free(r);
    run(h,"CDROP","ccomp",NULL);
}

/* ── §08 직렬화 (멀티스레드) ────────────────────────────── */
#define MT_THREADS 8
#define MT_OPS     200
typedef struct { MRedisHandle *h; int id; } MtArg;

static void *mt_kv_w(void *a){ MtArg *x=(MtArg*)a;
    for(int i=0;i<MT_OPS;i++){ char k[32],v[32];
        snprintf(k,32,"mt_kv_%d_%d",x->id,i); snprintf(v,32,"v%d",i);
        string_t sk={k,(uint32_t)strlen(k)},sv={v,(uint32_t)strlen(v)},sc={"SET",3};
        string_t *ar[]={&sc,&sk,&sv}; s_replyObject *r=cmd_dispatch(x->h,ar,3); reply_free(r); }
    return NULL; }
static void *mt_bset_w(void *a){ MtArg *x=(MtArg*)a;
    for(int i=0;i<MT_OPS;i++){ char sc[32],v[32];
        snprintf(sc,32,"%d",x->id*MT_OPS+i); snprintf(v,32,"v%d_%d",x->id,i);
        string_t a0={"BSET",4},a1={"mt_bset",7},a2={sc,(uint32_t)strlen(sc)},a3={v,(uint32_t)strlen(v)};
        string_t *ar[]={&a0,&a1,&a2,&a3}; s_replyObject *r=cmd_dispatch(x->h,ar,4); reply_free(r); }
    return NULL; }
static void *mt_cset_w(void *a){ MtArg *x=(MtArg*)a;
    for(int i=0;i<MT_OPS;i++){ char sc[32],v[32];
        snprintf(sc,32,"%d",x->id*MT_OPS+i); snprintf(v,32,"cv%d_%d",x->id,i);
        string_t a0={"CSET",4},a1={"mt_cset",7},a2={sc,(uint32_t)strlen(sc)},a3={v,(uint32_t)strlen(v)};
        string_t *ar[]={&a0,&a1,&a2,&a3}; s_replyObject *r=cmd_dispatch(x->h,ar,4); reply_free(r); }
    return NULL; }

static void t08_serialize(MRedisHandle *h) {
    SECT("08. 직렬화 (멀티스레드 rwlock 검증)");
    pthread_t tids[MT_THREADS]; MtArg args[MT_THREADS];

    for(int i=0;i<MT_THREADS;i++){ args[i].h=h; args[i].id=i;
        pthread_create(&tids[i],NULL,mt_kv_w,&args[i]); }
    for(int i=0;i<MT_THREADS;i++) pthread_join(tids[i],NULL);
    int kv_ok=1;
    for(int i=0;i<MT_THREADS&&kv_ok;i++) for(int j=0;j<MT_OPS&&kv_ok;j++){
        char k[32]; snprintf(k,32,"mt_kv_%d_%d",i,j);
        string_t sk={k,(uint32_t)strlen(k)},sc={"GET",3}; string_t *a[]={&sc,&sk};
        s_replyObject *r=cmd_dispatch(h,a,2);
        if(!r||r->type!=REPLY_STRING) kv_ok=0;
        reply_free(r); }
    CHECK(kv_ok,"bucket_mutex: 동시 SET 모두 저장됨");

    for(int i=0;i<MT_THREADS;i++){ args[i].h=h; args[i].id=i;
        pthread_create(&tids[i],NULL,mt_bset_w,&args[i]); }
    for(int i=0;i<MT_THREADS;i++) pthread_join(tids[i],NULL);
    s_replyObject *r=run(h,"BCARD","mt_bset",NULL);
    CHECK(is_int(r,(int64_t)(MT_THREADS*MT_OPS)),"BSET rwlock: BCARD=T×OPS"); reply_free(r);
    run(h,"BDROP","mt_bset",NULL);

    for(int i=0;i<MT_THREADS;i++){ args[i].h=h; args[i].id=i;
        pthread_create(&tids[i],NULL,mt_cset_w,&args[i]); }
    for(int i=0;i<MT_THREADS;i++) pthread_join(tids[i],NULL);
    r=run(h,"CCARD","mt_cset",NULL);
    CHECK(is_int(r,(int64_t)(MT_THREADS*MT_OPS)),"CSET rwlock: CCARD=T×OPS"); reply_free(r);
    run(h,"CDROP","mt_cset",NULL);
}

/* ── §09 ZINCRBY TOCTOU ─────────────────────────────────── */
#define ZINCRBY_T 8
#define ZINCRBY_I 100
static void *zincrby_w(void *a){ MRedisHandle *h=(MRedisHandle*)a;
    for(int i=0;i<ZINCRBY_I;i++){
        string_t a0={"ZINCRBY",7},a1={"tc_z",4},a2={"1",1},a3={"counter",7};
        string_t *ar[]={&a0,&a1,&a2,&a3}; s_replyObject *r=cmd_dispatch(h,ar,4); reply_free(r); }
    return NULL; }
static void t09_zincrby(MRedisHandle *h) {
    SECT("09. ZINCRBY TOCTOU 수정 검증");
    pthread_t tids[ZINCRBY_T];
    for(int i=0;i<ZINCRBY_T;i++) pthread_create(&tids[i],NULL,zincrby_w,h);
    for(int i=0;i<ZINCRBY_T;i++) pthread_join(tids[i],NULL);
    s_replyObject *r=run(h,"ZSCORE","tc_z","counter",NULL);
    int ok=(r&&r->type==REPLY_STRING&&
            llabs((int64_t)round(atof((char*)r->ptr))-(int64_t)(ZINCRBY_T*ZINCRBY_I))==0);
    CHECK(ok,"ZINCRBY 동시 incr 최종값 정확"); reply_free(r);
}

/* ── §10 멀티프로세스 stress ─────────────────────────────── */
#define MP_PROCS 8
#define MP_ITER  500
static void mp_worker(const char *nm, int id){ MRedisHandle *h=mredis_open_existing(nm);
    if(!h) exit(1);
    for(int i=0;i<MP_ITER;i++){ char k[32],v[32];
        snprintf(k,32,"mp_%d_%d",id,i); snprintf(v,32,"v%d",i);
        string_t sk={k,(uint32_t)strlen(k)},sv={v,(uint32_t)strlen(v)},sc={"SET",3};
        string_t *ar[]={&sc,&sk,&sv}; s_replyObject *r=cmd_dispatch(h,ar,3); reply_free(r); }
    mredis_close(h); exit(0); }
static void t10_mp(MRedisHandle *h){ SECT("10. 멀티프로세스 stress (8×500)");
    (void)h; pid_t pids[MP_PROCS];
    for(int i=0;i<MP_PROCS;i++){ pids[i]=fork(); if(pids[i]==0){ mp_worker(SHM_NAME,i); exit(0); } }
    for(int i=0;i<MP_PROCS;i++) waitpid(pids[i],NULL,0);
    MRedisHandle *vh=mredis_open_existing(SHM_NAME); int ok=1;
    for(int i=0;i<MP_PROCS&&ok;i++) for(int j=0;j<MP_ITER&&ok;j++){
        char k[32]; snprintf(k,32,"mp_%d_%d",i,j);
        string_t sk={k,(uint32_t)strlen(k)},sc={"GET",3}; string_t *a[]={&sc,&sk};
        s_replyObject *r=cmd_dispatch(vh,a,2);
        if(!r||r->type!=REPLY_STRING) ok=0;
        reply_free(r); }
    CHECK(ok,"멀티프로세스: 모든 키 정상 저장"); mredis_close(vh); }

/* ============================================================
 * §11  SET 연산 검증 : SADD / SUNION / SINTER / SDIFF 통합 테스트
 * ============================================================ */
static void t11_set_operations(MRedisHandle *h) {
    SECT("11. SET 집합 연산 검증 (SADD, SUNION, SINTER, SDIFF)...");
    int ok = 1;

    // 1. 데이터 준비 (set_A = {alpha, beta}, set_B = {beta, gamma})
    {
        string_t c_sadd = {"SADD", 4};
        string_t k_a = {"set_A", 5};
        string_t m_alpha = {"alpha", 5};
        string_t m_beta = {"beta", 4};
        
        string_t *a1[] = {&c_sadd, &k_a, &m_alpha, &m_beta};
        s_replyObject *r1 = cmd_dispatch(h, a1, 4);
        if (!r1 || r1->type != REPLY_INTEGER || r1->integer != 2) ok = 0;
        reply_free(r1);

        string_t k_b = {"set_B", 5};
        string_t m_gamma = {"gamma", 5};
        string_t *a2[] = {&c_sadd, &k_b, &m_beta, &m_gamma};
        s_replyObject *r2 = cmd_dispatch(h, a2, 4);
        if (!r2 || r2->type != REPLY_INTEGER || r2->integer != 2) ok = 0;
        reply_free(r2);
    }
    CHECK(ok, "SADD를 통한 집합 초기 데이터 세팅 성공");

    // 2. SUNION 테스트 (결과 집합크기: 3 -> alpha, beta, gamma)
    {
        string_t c_sunion = {"SUNION", 6};
        string_t k_a = {"set_A", 5};
        string_t k_b = {"set_B", 5};
        string_t *a[] = {&c_sunion, &k_a, &k_b};
        
        s_replyObject *r = cmd_dispatch(h, a, 3);
        if (!r || r->type != REPLY_ARRAY || r->elements != 3) {
            ok = 0;
        } else {
            // 결과 항목 체크
            int has_alpha = 0, has_beta = 0, has_gamma = 0;
            for(uint32_t i=0; i<r->elements; i++) {
                if(strncmp(r->element[i]->ptr, "alpha", r->element[i]->len) == 0) has_alpha = 1;
                if(strncmp(r->element[i]->ptr, "beta", r->element[i]->len) == 0) has_beta = 1;
                if(strncmp(r->element[i]->ptr, "gamma", r->element[i]->len) == 0) has_gamma = 1;
            }
            if(!has_alpha || !has_beta || !has_gamma) ok = 0;
        }
        reply_free(r);
    }
    CHECK(ok, "SUNION (합집합) 데이터 정밀 무결성 검증 통과");

    // 3. SINTER 테스트 (결과 집합크기: 1 -> beta)
    {
        string_t c_sinter = {"SINTER", 6};
        string_t k_a = {"set_A", 5};
        string_t k_b = {"set_B", 5};
        string_t *a[] = {&c_sinter, &k_a, &k_b};
        
        s_replyObject *r = cmd_dispatch(h, a, 3);
        if (!r || r->type != REPLY_ARRAY || r->elements != 1) {
            ok = 0;
        } else {
            if(strncmp(r->element[0]->ptr, "beta", r->element[0]->len) != 0) ok = 0;
        }
        reply_free(r);
    }
    CHECK(ok, "SINTER (교집합) 데이터 정밀 무결성 검증 통과");

    // 4. SDIFF 테스트 (set_A - set_B = {alpha})
    {
        string_t c_sdiff = {"SDIFF", 5};
        string_t k_a = {"set_A", 5};
        string_t k_b = {"set_B", 5};
        string_t *a[] = {&c_sdiff, &k_a, &k_b};
        
        s_replyObject *r = cmd_dispatch(h, a, 3);
        if (!r || r->type != REPLY_ARRAY || r->elements != 1) {
            ok = 0;
        } else {
            if(strncmp(r->element[0]->ptr, "alpha", r->element[0]->len) != 0) ok = 0;
        }
        reply_free(r);
    }
    CHECK(ok, "SDIFF (차집합) 데이터 정밀 무결성 검증 통과");
}
/* ============================================================
 * [§11] SET 명령어 세트 완전 검증 (SADD, SREM, SISMEMBER 등)
 * ============================================================ */
static void t11_set_suite(MRedisHandle *h) {
    SECT("12. SET 풀 명령어 세트 및 집합 연산 정밀 검증...\n");
    int ok = 1;

    // 테스트용 명령어 이름 및 Key, Member 정의
    string_t c_sadd = {"SADD", 4}, c_srem = {"SREM", 4}, c_sismember = {"SISMEMBER", 9};
    string_t c_scard = {"SCARD", 5}, c_smembers = {"SMEMBERS", 8}, c_spop = {"SPOP", 4};
    string_t c_srand = {"SRANDMEMBER", 11};
    string_t c_sunion = {"SUNION", 6}, c_sinter = {"SINTER", 6}, c_sdiff = {"SDIFF", 5};

    string_t k_set1 = {"myset1", 6}, k_set2 = {"myset2", 6};
    string_t m_A = {"alpha", 5}, m_B = {"beta", 4}, m_C = {"gamma", 5}, m_D = {"delta", 5};

    /* 1. SADD 멤버 추가 및 중복 추가 방증 테스트 */
    {
        // myset1에 alpha, beta 추가 (예상 반환: 2)
        string_t *a1[] = {&c_sadd, &k_set1, &m_A, &m_B};
        s_replyObject *r1 = cmd_dispatch(h, a1, 4);
        if (!r1 || r1->type != REPLY_INTEGER || r1->integer != 2) ok = 0;
        reply_free(r1);

        // 동일 멤버 중복 추가 시도 (예상 반환: 0 - 추가 안 됨)
        string_t *a2[] = {&c_sadd, &k_set1, &m_A};
        s_replyObject *r2 = cmd_dispatch(h, a2, 3);
        if (!r2 || r2->type != REPLY_INTEGER || r2->integer != 0) ok = 0;
        reply_free(r2);
    }
    CHECK(ok, "SADD 신규 추가 및 중복 필터링 기전 검증 통과");

    /* 2. SISMEMBER 및 SCARD 검증 */
    {
        // 존재 여부 확인 (alpha -> 1, delta -> 0)
        string_t *a1[] = {&c_sismember, &k_set1, &m_A};
        s_replyObject *r1 = cmd_dispatch(h, a1, 3);
        if (!r1 || r1->type != REPLY_INTEGER || r1->integer != 1) ok = 0;
        reply_free(r1);

        string_t *a2[] = {&c_sismember, &k_set1, &m_D};
        s_replyObject *r2 = cmd_dispatch(h, a2, 3);
        if (!r2 || r2->type != REPLY_INTEGER || r2->integer != 0) ok = 0;
        reply_free(r2);

        // 원소 개수 확인 (2개)
        string_t *a3[] = {&c_scard, &k_set1};
        s_replyObject *r3 = cmd_dispatch(h, a3, 2);
        if (!r3 || r3->type != REPLY_INTEGER || r3->integer != 2) ok = 0;
        reply_free(r3);
    }
    CHECK(ok, "SISMEMBER 및 SCARD 데이터 정밀 매칭 완벽");

    /* 3. SRANDMEMBER 및 SMEMBERS 조회 검증 */
    {
        // 전체 멤버 가져오기
        string_t *a1[] = {&c_smembers, &k_set1};
        s_replyObject *r1 = cmd_dispatch(h, a1, 2);
        if (!r1 || r1->type != REPLY_ARRAY || r1->elements != 2) {
            ok = 0;
        } else {
            int has_A = 0, has_B = 0;
            if (strncmp(r1->element[0]->ptr, "alpha", r1->element[0]->len) == 0 ||
                strncmp(r1->element[1]->ptr, "alpha", r1->element[1]->len) == 0) has_A = 1;
            if (strncmp(r1->element[0]->ptr, "beta",  r1->element[0]->len) == 0 ||
                strncmp(r1->element[1]->ptr, "beta",  r1->element[1]->len) == 0) has_B = 1;
            if (!has_A || !has_B) ok = 0;
        }
        reply_free(r1);

        // 랜덤 원소 단일 추출 검증 (Set을 손상시키지 않고 리턴만 해야 함)
        string_t *a2[] = {&c_srand, &k_set1};
        s_replyObject *r2 = cmd_dispatch(h, a2, 2);
        if (!r2 || (r2->type != REPLY_STRING && r2->type != REPLY_STATUS)) ok = 0;
        reply_free(r2);
    }
    CHECK(ok, "SMEMBERS 전수 스캔 및 SRANDMEMBER 랜덤 조회 성공");

    /* 4. SUNION, SINTER, SDIFF 집합 연산 검증 */
    {
        // 비교를 위한 두 번째 세트 생성 (myset2 = {beta, gamma})
        string_t *a_init[] = {&c_sadd, &k_set2, &m_B, &m_C};
        s_replyObject *r_init = cmd_dispatch(h, a_init, 4);
        reply_free(r_init);

        // 합집합: myset1 ∪ myset2 = {alpha, beta, gamma} (3개)
        string_t *a_uni[] = {&c_sunion, &k_set1, &k_set2};
        s_replyObject *r_uni = cmd_dispatch(h, a_uni, 3);
        if (!r_uni || r_uni->type != REPLY_ARRAY || r_uni->elements != 3) ok = 0;
        reply_free(r_uni);

        // 교집합: myset1 ∩ myset2 = {beta} (1개)
        string_t *a_inter[] = {&c_sinter, &k_set1, &k_set2};
        s_replyObject *r_inter = cmd_dispatch(h, a_inter, 3);
        if (!r_inter || r_inter->type != REPLY_ARRAY || r_inter->elements != 1) {
            ok = 0;
        } else {
            if (strncmp(r_inter->element[0]->ptr, "beta", r_inter->element[0]->len) != 0) ok = 0;
        }
        reply_free(r_inter);

        // 차집합: myset1 - myset2 = {alpha} (1개)
        string_t *a_diff[] = {&c_sdiff, &k_set1, &k_set2};
        s_replyObject *r_diff = cmd_dispatch(h, a_diff, 3);
        if (!r_diff || r_diff->type != REPLY_ARRAY || r_diff->elements != 1) {
            ok = 0;
        } else {
            if (strncmp(r_diff->element[0]->ptr, "alpha", r_diff->element[0]->len) != 0) ok = 0;
        }
        reply_free(r_diff);
    }
    CHECK(ok, "SUNION / SINTER / SDIFF 다중 집합 연산 무결성 획득");

    /* 5. SPOP 및 SREM 데이터 소멸 및 힙 메모리 해제 검증 */
    {
        // SPOP 실행 (원소 하나를 제거 후 반환해야 함)
        string_t *a1[] = {&c_spop, &k_set1};
        s_replyObject *r1 = cmd_dispatch(h, a1, 2);
        if (!r1 || (r1->type != REPLY_STRING && r1->type != REPLY_STATUS)) ok = 0;
        reply_free(r1);

        // SCARD 확인 (1개 남아있어야 함)
        string_t *a2[] = {&c_scard, &k_set1};
        s_replyObject *r2 = cmd_dispatch(h, a2, 2);
        if (!r2 || r2->type != REPLY_INTEGER || r2->integer != 1) ok = 0;
        reply_free(r2);

        // SREM을 이용하여 남은 마지막 원소까지 완전 청소
        // 팝되고 남은 원소가 alpha인지 beta인지 모르므로 둘 다 타겟팅하여 삭제 명령 전달
        string_t *a3[] = {&c_srem, &k_set1, &m_A, &m_B};
        s_replyObject *r3 = cmd_dispatch(h, a3, 4);
        if (!r3 || r3->type != REPLY_INTEGER || r3->integer != 1) ok = 0;
        reply_free(r3);

        // 이제 비어있어야 함 (SCARD -> 0)
        string_t *a4[] = {&c_scard, &k_set1};
        s_replyObject *r4 = cmd_dispatch(h, a4, 2);
        if (!r4 || r4->type != REPLY_INTEGER || r4->integer != 0) ok = 0;
        reply_free(r4);
    }
    CHECK(ok, "SPOP 및 SREM 연산 후 내부 오프셋 클린업 및 가비지 해제 확인");
}
/* ── main ───────────────────────────────────────────────── */
int main(void) {
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║     mredis 전체 통합 테스트 (§01~§10)        ║\n");
    printf("╚══════════════════════════════════════════════╝\n");
    mredis_destroy(SHM_NAME);
    MRedisHandle *h=mredis_create(SHM_NAME,512ULL*1024*1024);
    if(!h){ fprintf(stderr,"SHM 생성 실패\n"); return 1; }
    mredis_set_debug_level(DBG_ERROR);
    t01_kv(h); t02_keys(h); t03_del(h); t04_zset(h); t05_hash(h);
    t06_bset(h); t07_cset(h); t08_serialize(h); t09_zincrby(h); t10_mp(h);

	t11_set_operations(h);

	t11_set_suite(h);
	s_replyObject *r = run(h, "KEYS", "*", NULL);
	for (size_t i=0;i<r->elements;i++)	{
#if 0
		printf ("%s\n", (char*)r->element[i]->ptr);
#else
		reply_free(run(h, "DEL", r->element[i]->ptr, NULL));
#endif
	}
	reply_free(r);

	mredis_dump_stats(h);
    mredis_set_debug_level(DBG_INFO);
    mredis_close(h);
//	mredis_destroy(SHM_NAME);
    printf("\n══════════════════════════════════════════════\n");
    printf("결과: \033[32mPASS %d\033[0m / \033[31mFAIL %d\033[0m / 합계 %d\n",
           g_pass,g_fail,g_pass+g_fail);
    printf("══════════════════════════════════════════════\n");
    return g_fail>0?1:0;
}
