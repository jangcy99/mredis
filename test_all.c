/*
 * test_all.c  –  통합 테스트 (s_replyObject / cmd_dispatch)
 *
 *  모든 커맨드를 cmd_dispatch() 를 통해 호출하고
 *  반환된 s_replyObject 로 결과를 검증한다.
 *
 *  1)  KV SET / GET / DEL
 *  2)  KV 값 갱신
 *  3)  KV 없는 키 → NIL
 *  4)  키 전역 유일 – KV → ZSet 중복
 *  5)  키 전역 유일 – KV → Hash 중복
 *  6)  키 전역 유일 – ZSet → KV 중복
 *  7)  키 전역 유일 – Hash → ZSet 중복
 *  8)  ZSet ZADD / ZSCORE / ZREM
 *  9)  ZSet ZADD 플래그 NX / XX / GT / LT / CH
 * 10)  ZSet ZINCRBY / ZRANK / ZREVRANK / ZCARD / ZCOUNT
 * 11)  ZSet ZRANGE / ZRANGEBYSCORE / ZPOPMIN / ZPOPMAX
 * 12)  Hash HSET / HGET / HDEL / HEXISTS / HLEN
 * 13)  Hash HGETALL / HKEYS / HVALS
 * 14)  Hash HINCRBY / HINCRBYFLOAT
 * 15)  Hash 타입 불일치
 * 16)  Hash 대량 field
 * 17)  ZSet DROP 후 재생성
 * 18)  cmd_table_get – 커맨드 테이블 확인
 * 19)  reply_print 출력 검증 (시각 확인용)
 * 20)  멀티프로세스 동시 SET + HSET + ZADD
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>
#include <sys/wait.h>

#include "shm_types.h"
#include "shm_core.h"
#include "cmd_dispatch.h"

/* ─────────────────────────────────────────────────────────
 *  테스트 유틸
 * ───────────────────────────────────────────────────────── */
#define SHM_TEST    "/shm_v5_test"
#define PASS        "\033[32m[PASS]\033[0m"
#define FAIL        "\033[31m[FAIL]\033[0m"
#define SECT(t)     printf("\n\033[36m--- %s ---\033[0m\n", (t))

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) \
    do { if(cond){printf(PASS " %s\n",(msg));g_pass++;} \
         else    {printf(FAIL " %s\n",(msg));g_fail++;} } while(0)

/* ── string_t 배열 기반 dispatch 헬퍼 ─────────────────────
 *  가변 인자 문자열 → dispatch 호출 → s_replyObject* 반환
 *  사용: run(h, "SET", "key", "val", NULL)
 * ───────────────────────────────────────────────────────── */
static s_replyObject *run(ShmHandle *h, ...)
{
    /* 최대 32 인자 */
    string_t  *args[32];
    string_t   bufs[32];
    uint32_t   argc = 0;

    va_list ap; va_start(ap, h);
    while (argc < 32) {
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

/* 반환값 타입 확인 헬퍼 */
static int is_ok   (s_replyObject *r){ return r && r->type==REPLY_STATUS && r->ptr && strcmp((char*)r->ptr,"OK")==0; }
static int is_nil  (s_replyObject *r){ return r && r->type==REPLY_NIL; }
static int is_int  (s_replyObject *r, int64_t v){ return r && r->type==REPLY_INTEGER && r->integer==v; }
static int is_str  (s_replyObject *r, const char *s){ return r && r->type==REPLY_STRING && r->ptr && strcmp((char*)r->ptr,s)==0; }
static int is_err  (s_replyObject *r, int code){ return r && r->type==REPLY_ERROR && r->integer==code; }
static int is_arr  (s_replyObject *r, size_t n){ return r && r->type==REPLY_ARRAY && r->elements==n; }
static double to_dbl(s_replyObject *r){ return r&&r->type==REPLY_STRING&&r->ptr?strtod((char*)r->ptr,NULL):0.0; }

/* ─────────────────────────────────────────────────────────
 *  1. KV 기본 CRUD
 * ───────────────────────────────────────────────────────── */
static void test_kv_basic(ShmHandle *h)
{
    SECT("1. KV 기본 CRUD");
    s_replyObject *r;

    r = run(h,"SET","hello","world",NULL); CHECK(is_ok(r),"SET hello world"); reply_free(r);
    r = run(h,"SET","foo","bar_12345",NULL); CHECK(is_ok(r),"SET foo bar"); reply_free(r);
    r = run(h,"SET","empty","",NULL); CHECK(is_ok(r),"SET empty ''"); reply_free(r);

    r = run(h,"GET","hello",NULL); CHECK(is_str(r,"world"),"GET hello==world"); reply_free(r);
    r = run(h,"GET","foo",NULL);   CHECK(is_str(r,"bar_12345"),"GET foo==bar"); reply_free(r);
    r = run(h,"GET","empty",NULL); CHECK(r&&r->type==REPLY_STRING&&r->len==0,"GET empty len==0"); reply_free(r);

    r = run(h,"DEL","hello",NULL); CHECK(is_int(r,1),"DEL hello →1"); reply_free(r);
    r = run(h,"GET","hello",NULL); CHECK(is_nil(r),"GET hello after DEL →NIL"); reply_free(r);
    r = run(h,"DEL","no_key",NULL); CHECK(is_int(r,0),"DEL 없는 키 →0"); reply_free(r);

    /* 복수 키 삭제 */
    run(h,"SET","k1","v1",NULL); run(h,"SET","k2","v2",NULL);
    r = run(h,"DEL","k1","k2","k3",NULL); /* k3 없음 */
    CHECK(is_int(r,2),"DEL 복수 키 →2"); reply_free(r);

    run(h,"DEL","foo",NULL); run(h,"DEL","empty",NULL);
}

/* ─────────────────────────────────────────────────────────
 *  2. KV 값 갱신
 * ───────────────────────────────────────────────────────── */
static void test_kv_update(ShmHandle *h)
{
    SECT("2. KV 값 갱신");
    s_replyObject *r;
    run(h,"SET","upk","first",NULL);
    run(h,"SET","upk","second_longer_value",NULL);
    r = run(h,"GET","upk",NULL); CHECK(is_str(r,"second_longer_value"),"갱신 후 새 값"); reply_free(r);
    run(h,"SET","upk","x",NULL);
    r = run(h,"GET","upk",NULL); CHECK(is_str(r,"x"),"짧은 값 재갱신"); reply_free(r);
    run(h,"DEL","upk",NULL);
}

/* ─────────────────────────────────────────────────────────
 *  3. KV 없는 키 → NIL
 * ───────────────────────────────────────────────────────── */
static void test_kv_notfound(ShmHandle *h)
{
    SECT("3. KV 없는 키");
    s_replyObject *r;
    r = run(h,"GET","ghost",NULL);    CHECK(is_nil(r),"GET ghost →NIL"); reply_free(r);
    r = run(h,"DEL","ghost",NULL);    CHECK(is_int(r,0),"DEL ghost →0"); reply_free(r);
}

/* ─────────────────────────────────────────────────────────
 *  4~7. 키 전역 유일 강제
 * ───────────────────────────────────────────────────────── */
static void test_key_unique(ShmHandle *h)
{
    SECT("4. 키 전역 유일 – KV → ZSet 중복");
    s_replyObject *r;
    run(h,"SET","dup_kv","v",NULL);
    r = run(h,"ZCREATE","dup_kv",NULL);
    CHECK(is_err(r,SHM_ERR_KEY_EXISTS),"KV키로 ZCREATE →KEY_EXISTS"); reply_free(r);
    r = run(h,"GET","dup_kv",NULL); CHECK(is_str(r,"v"),"KV 무결성 유지"); reply_free(r);
    run(h,"DEL","dup_kv",NULL);

    SECT("5. 키 전역 유일 – KV → Hash 중복");
    run(h,"SET","dup_kv2","v2",NULL);
    r = run(h,"HCREATE","dup_kv2",NULL);
    CHECK(is_err(r,SHM_ERR_KEY_EXISTS),"KV키로 HCREATE →KEY_EXISTS"); reply_free(r);
    run(h,"DEL","dup_kv2",NULL);

    SECT("6. 키 전역 유일 – ZSet → KV 중복");
    run(h,"ZCREATE","dup_zs",NULL);
    r = run(h,"SET","dup_zs","v",NULL);
    CHECK(is_err(r,SHM_ERR_KEY_EXISTS),"ZSet이름으로 SET →KEY_EXISTS"); reply_free(r);
    run(h,"ZADD","dup_zs","1.0","m",NULL);
    r = run(h,"ZCARD","dup_zs",NULL); CHECK(is_int(r,1),"ZSet 무결성 유지"); reply_free(r);
    run(h,"ZDROP","dup_zs",NULL);

    SECT("7. 키 전역 유일 – Hash → ZSet 중복");
    run(h,"HCREATE","dup_hh",NULL);
    run(h,"HSET","dup_hh","f1","v1",NULL);
    r = run(h,"ZCREATE","dup_hh",NULL);
    CHECK(is_err(r,SHM_ERR_KEY_EXISTS),"Hash이름으로 ZCREATE →KEY_EXISTS"); reply_free(r);
    r = run(h,"HEXISTS","dup_hh","f1",NULL); CHECK(is_int(r,1),"Hash 무결성 유지"); reply_free(r);
    run(h,"HDROP","dup_hh",NULL);
}

/* ─────────────────────────────────────────────────────────
 *  8. ZSet ZADD / ZSCORE / ZREM
 * ───────────────────────────────────────────────────────── */
static void test_zset_basic(ShmHandle *h)
{
    SECT("8. ZSet ZADD / ZSCORE / ZREM");
    s_replyObject *r;

    run(h,"ZCREATE","sc",NULL);
    r = run(h,"ZADD","sc","1.0","alice",NULL); CHECK(is_int(r,1),"ZADD alice →1"); reply_free(r);
    r = run(h,"ZADD","sc","2.5","bob",NULL);   CHECK(is_int(r,1),"ZADD bob →1"); reply_free(r);
    r = run(h,"ZADD","sc","0.5","eve",NULL);   CHECK(is_int(r,1),"ZADD eve →1"); reply_free(r);

    r = run(h,"ZSCORE","sc","alice",NULL); CHECK(r&&fabs(to_dbl(r)-1.0)<1e-9,"ZSCORE alice==1.0"); reply_free(r);
    r = run(h,"ZSCORE","sc","bob",NULL);   CHECK(r&&fabs(to_dbl(r)-2.5)<1e-9,"ZSCORE bob==2.5"); reply_free(r);
    r = run(h,"ZSCORE","sc","ghost",NULL); CHECK(is_nil(r),"ZSCORE ghost →NIL"); reply_free(r);

    /* score 갱신 */
    r = run(h,"ZADD","sc","9.9","alice",NULL); CHECK(is_int(r,0),"ZADD alice 갱신 →0"); reply_free(r);
    r = run(h,"ZSCORE","sc","alice",NULL); CHECK(r&&fabs(to_dbl(r)-9.9)<1e-9,"ZSCORE alice==9.9"); reply_free(r);

    r = run(h,"ZREM","sc","bob",NULL); CHECK(is_int(r,1),"ZREM bob →1"); reply_free(r);
    r = run(h,"ZCARD","sc",NULL);      CHECK(is_int(r,2),"ZCARD==2 after ZREM"); reply_free(r);
    r = run(h,"ZREM","sc","ghost",NULL); CHECK(is_int(r,0),"ZREM ghost →0"); reply_free(r);

    run(h,"ZDROP","sc",NULL);
}

/* ─────────────────────────────────────────────────────────
 *  9. ZSet ZADD 플래그
 * ───────────────────────────────────────────────────────── */
static void test_zset_flags(ShmHandle *h)
{
    SECT("9. ZSet ZADD 플래그 NX/XX/GT/LT/CH");
    s_replyObject *r;
    run(h,"ZCREATE","fl",NULL);
    run(h,"ZADD","fl","5.0","m",NULL);

    r = run(h,"ZADD","fl","NX","99.0","m",NULL);  CHECK(is_int(r,0),"NX 이미 있음 →0"); reply_free(r);
    r = run(h,"ZSCORE","fl","m",NULL); CHECK(r&&fabs(to_dbl(r)-5.0)<1e-9,"NX 후 score 불변"); reply_free(r);

    r = run(h,"ZADD","fl","NX","3.0","newm",NULL); CHECK(is_int(r,1),"NX 없는 멤버 →1"); reply_free(r);

    r = run(h,"ZADD","fl","XX","7.0","ghost",NULL); CHECK(is_int(r,0),"XX 없음 →0"); reply_free(r);
    r = run(h,"ZSCORE","fl","ghost",NULL); CHECK(is_nil(r),"XX ghost 미생성"); reply_free(r);

    run(h,"ZADD","fl","XX","8.0","m",NULL);
    r = run(h,"ZSCORE","fl","m",NULL); CHECK(r&&fabs(to_dbl(r)-8.0)<1e-9,"XX 갱신 →8.0"); reply_free(r);

    r = run(h,"ZADD","fl","GT","1.0","m",NULL); CHECK(is_int(r,0),"GT 더 작음 →0"); reply_free(r);
    r = run(h,"ZSCORE","fl","m",NULL); CHECK(r&&fabs(to_dbl(r)-8.0)<1e-9,"GT 후 불변"); reply_free(r);
    run(h,"ZADD","fl","GT","20.0","m",NULL);
    r = run(h,"ZSCORE","fl","m",NULL); CHECK(r&&fabs(to_dbl(r)-20.0)<1e-9,"GT 적용 →20"); reply_free(r);

    r = run(h,"ZADD","fl","LT","99.0","m",NULL); CHECK(is_int(r,0),"LT 더 큰 값 →0"); reply_free(r);
    run(h,"ZADD","fl","LT","0.1","m",NULL);
    r = run(h,"ZSCORE","fl","m",NULL); CHECK(r&&fabs(to_dbl(r)-0.1)<1e-9,"LT 적용 →0.1"); reply_free(r);

    r = run(h,"ZADD","fl","CH","42.0","m",NULL); CHECK(is_int(r,1),"CH 변경 →1"); reply_free(r);
    r = run(h,"ZADD","fl","CH","42.0","m",NULL); CHECK(is_int(r,0),"CH 동일 →0"); reply_free(r);

    run(h,"ZDROP","fl",NULL);
}

/* ─────────────────────────────────────────────────────────
 *  10. ZSet ZINCRBY / ZRANK / ZREVRANK / ZCARD / ZCOUNT
 * ───────────────────────────────────────────────────────── */
static void test_zset_misc(ShmHandle *h)
{
    SECT("10. ZSet ZINCRBY / ZRANK / ZREVRANK / ZCARD / ZCOUNT");
    s_replyObject *r;
    run(h,"ZCREATE","ms",NULL);
    run(h,"ZADD","ms","1.0","a",NULL);
    run(h,"ZADD","ms","2.0","b",NULL);
    run(h,"ZADD","ms","3.0","c",NULL);
    run(h,"ZADD","ms","4.0","d",NULL);

    r = run(h,"ZINCRBY","ms","5.0","a",NULL);
    CHECK(r&&r->type==REPLY_STRING&&fabs(to_dbl(r)-6.0)<1e-9,"ZINCRBY a+5 →6.0"); reply_free(r);

    r = run(h,"ZINCRBY","ms","1.0","new_m",NULL);
    CHECK(r&&r->type==REPLY_STRING&&fabs(to_dbl(r)-1.0)<1e-9,"ZINCRBY new_m auto-create →1.0"); reply_free(r);

    /* 정렬: new_m=1, b=2, c=3, d=4, a=6 */
    r = run(h,"ZRANK","ms","new_m",NULL); CHECK(is_int(r,0),"ZRANK new_m==0"); reply_free(r);
    r = run(h,"ZRANK","ms","b",NULL);     CHECK(is_int(r,1),"ZRANK b==1"); reply_free(r);
    r = run(h,"ZRANK","ms","ghost",NULL); CHECK(is_nil(r),"ZRANK ghost →NIL"); reply_free(r);
    r = run(h,"ZREVRANK","ms","a",NULL);  CHECK(is_int(r,0),"ZREVRANK a==0(최대)"); reply_free(r);

    r = run(h,"ZCARD","ms",NULL);         CHECK(is_int(r,5),"ZCARD==5"); reply_free(r);
    r = run(h,"ZCOUNT","ms","1","4",NULL);CHECK(is_int(r,4),"ZCOUNT [1,4]==4"); reply_free(r);
    r = run(h,"ZCOUNT","ms","0","100",NULL); CHECK(is_int(r,5),"ZCOUNT [0,100]==5"); reply_free(r);

    run(h,"ZDROP","ms",NULL);
}

/* ─────────────────────────────────────────────────────────
 *  11. ZSet ZRANGE / ZRANGEBYSCORE / ZPOPMIN / ZPOPMAX
 * ───────────────────────────────────────────────────────── */
static void test_zset_range(ShmHandle *h)
{
    SECT("11. ZRANGE / ZRANGEBYSCORE / ZPOPMIN / ZPOPMAX");
    s_replyObject *r;
    run(h,"ZCREATE","rng",NULL);
    run(h,"ZADD","rng","1","alpha",NULL);
    run(h,"ZADD","rng","2","beta",NULL);
    run(h,"ZADD","rng","3","gamma",NULL);
    run(h,"ZADD","rng","4","delta",NULL);
    run(h,"ZADD","rng","5","epsilon",NULL);

    /* ZRANGE ASC 전체 → ARRAY [alpha 1 beta 2 … epsilon 5] */
    r = run(h,"ZRANGE","rng","0","-1",NULL);
    CHECK(is_arr(r,10),"ZRANGE ASC 전체 10 원소");
    if(r&&r->elements>=2) CHECK(!strcmp((char*)r->element[0]->ptr,"alpha"),"첫 멤버==alpha");
    reply_free(r);

    /* ZRANGE REV */
    r = run(h,"ZRANGE","rng","0","-1","REV",NULL);
    CHECK(is_arr(r,10),"ZRANGE REV 10 원소");
    if(r&&r->elements>=1) CHECK(!strcmp((char*)r->element[0]->ptr,"epsilon"),"REV 첫==epsilon");
    reply_free(r);

    /* ZRANGE 음수 인덱스 [-2,-1] → delta, epsilon */
    r = run(h,"ZRANGE","rng","-2","-1",NULL);
    CHECK(is_arr(r,4),"ZRANGE [-2,-1] 4 원소(delta,epsilon)");
    if(r&&r->elements>=1) CHECK(!strcmp((char*)r->element[0]->ptr,"delta"),"[-2]==delta");
    reply_free(r);

    /* ZRANGEBYSCORE [2,4] */
    r = run(h,"ZRANGEBYSCORE","rng","2","4",NULL);
    CHECK(is_arr(r,6),"ZRANGEBYSCORE [2,4] 6 원소");
    reply_free(r);

    /* ZRANGEBYSCORE LIMIT offset=1 count=2 */
    r = run(h,"ZRANGEBYSCORE","rng","1","5","LIMIT","1","2",NULL);
    CHECK(is_arr(r,4),"ZRANGEBYSCORE LIMIT(1,2) 4 원소");
    if(r&&r->elements>=1) CHECK(!strcmp((char*)r->element[0]->ptr,"beta"),"LIMIT[0]==beta");
    reply_free(r);

    /* ZPOPMIN 2개 */
    r = run(h,"ZPOPMIN","rng","2",NULL);
    CHECK(is_arr(r,4),"ZPOPMIN 2 → 4 원소");
    if(r&&r->elements>=1) CHECK(!strcmp((char*)r->element[0]->ptr,"alpha"),"ZPOPMIN[0]==alpha");
    reply_free(r);
    r = run(h,"ZCARD","rng",NULL); CHECK(is_int(r,3),"ZCARD==3 after ZPOPMIN"); reply_free(r);

    /* ZPOPMAX 1개 */
    r = run(h,"ZPOPMAX","rng","1",NULL);
    CHECK(is_arr(r,2),"ZPOPMAX 1 → 2 원소");
    if(r&&r->elements>=1) CHECK(!strcmp((char*)r->element[0]->ptr,"epsilon"),"ZPOPMAX[0]==epsilon");
    reply_free(r);

    run(h,"ZDROP","rng",NULL);
}

/* ─────────────────────────────────────────────────────────
 *  12. Hash HSET / HGET / HDEL / HEXISTS / HLEN
 * ───────────────────────────────────────────────────────── */
static void test_hash_basic(ShmHandle *h)
{
    SECT("12. Hash HSET / HGET / HDEL / HEXISTS / HLEN");
    s_replyObject *r;

    run(h,"HCREATE","user:1",NULL);
    r = run(h,"HSET","user:1","name","Alice","age","30","email","a@b.com","score","99.5",NULL);
    CHECK(is_int(r,4),"HSET 4 신규 →4"); reply_free(r);

    r = run(h,"HSET","user:1","age","31",NULL); CHECK(is_int(r,0),"HSET age 갱신 →0"); reply_free(r);

    r = run(h,"HGET","user:1","name",NULL);  CHECK(is_str(r,"Alice"),"HGET name==Alice"); reply_free(r);
    r = run(h,"HGET","user:1","age",NULL);   CHECK(is_str(r,"31"),"HGET age==31"); reply_free(r);
    r = run(h,"HGET","user:1","ghost",NULL); CHECK(is_nil(r),"HGET ghost →NIL"); reply_free(r);

    r = run(h,"HEXISTS","user:1","email",NULL); CHECK(is_int(r,1),"HEXISTS email→1"); reply_free(r);
    r = run(h,"HEXISTS","user:1","phone",NULL); CHECK(is_int(r,0),"HEXISTS phone→0"); reply_free(r);
    r = run(h,"HLEN","user:1",NULL);            CHECK(is_int(r,4),"HLEN==4"); reply_free(r);

    r = run(h,"HDEL","user:1","email","score",NULL); CHECK(is_int(r,2),"HDEL 2개 →2"); reply_free(r);
    r = run(h,"HLEN","user:1",NULL); CHECK(is_int(r,2),"HLEN==2 after HDEL"); reply_free(r);
    r = run(h,"HDEL","user:1","ghost",NULL); CHECK(is_int(r,0),"HDEL ghost →0"); reply_free(r);

    run(h,"HDROP","user:1",NULL);
}

/* ─────────────────────────────────────────────────────────
 *  13. Hash HGETALL / HKEYS / HVALS
 * ───────────────────────────────────────────────────────── */
static void test_hash_getall(ShmHandle *h)
{
    SECT("13. Hash HGETALL / HKEYS / HVALS");
    s_replyObject *r;
    run(h,"HCREATE","item:1",NULL);
    run(h,"HSET","item:1","color","red","size","L","price","19.99",NULL);

    r = run(h,"HGETALL","item:1",NULL);
    CHECK(is_arr(r,6),"HGETALL 6 원소 (3쌍)");
    /* field 와 value 가 교대로 들어있는지 확인 */
    int found_red=0,found_L=0;
    if(r&&r->type==REPLY_ARRAY){
        for(size_t i=0;i+1<r->elements;i+=2){
            s_replyObject *fobj=r->element[i], *vobj=r->element[i+1];
            if(fobj&&vobj&&fobj->ptr&&vobj->ptr){
                if(!strcmp((char*)vobj->ptr,"red")) found_red=1;
                if(!strcmp((char*)vobj->ptr,"L"))   found_L=1;
            }
        }
    }
    CHECK(found_red,"HGETALL red 값 존재");
    CHECK(found_L,"HGETALL L 값 존재");
    reply_free(r);

    r = run(h,"HKEYS","item:1",NULL); CHECK(is_arr(r,3),"HKEYS 3 원소"); reply_free(r);
    r = run(h,"HVALS","item:1",NULL); CHECK(is_arr(r,3),"HVALS 3 원소"); reply_free(r);

    run(h,"HDROP","item:1",NULL);
}

/* ─────────────────────────────────────────────────────────
 *  14. Hash HINCRBY / HINCRBYFLOAT
 * ───────────────────────────────────────────────────────── */
static void test_hash_incr(ShmHandle *h)
{
    SECT("14. Hash HINCRBY / HINCRBYFLOAT");
    s_replyObject *r;
    run(h,"HCREATE","ctr",NULL);

    r = run(h,"HINCRBY","ctr","hits","10",NULL); CHECK(is_int(r,10),"HINCRBY hits 없음 →10"); reply_free(r);
    r = run(h,"HINCRBY","ctr","hits","5",NULL);  CHECK(is_int(r,15),"HINCRBY +5 →15"); reply_free(r);
    r = run(h,"HINCRBY","ctr","hits","-3",NULL); CHECK(is_int(r,12),"HINCRBY -3 →12"); reply_free(r);

    r = run(h,"HINCRBYFLOAT","ctr","rate","1.5",NULL);
    CHECK(r&&r->type==REPLY_STRING&&fabs(to_dbl(r)-1.5)<1e-9,"HINCRBYFLOAT 없음 →1.5"); reply_free(r);
    r = run(h,"HINCRBYFLOAT","ctr","rate","0.3",NULL);
    CHECK(r&&r->type==REPLY_STRING&&fabs(to_dbl(r)-1.8)<1e-9,"HINCRBYFLOAT +0.3 →1.8"); reply_free(r);

    run(h,"HDROP","ctr",NULL);
}

/* ─────────────────────────────────────────────────────────
 *  15. Hash 타입 불일치
 * ───────────────────────────────────────────────────────── */
static void test_hash_type_mismatch(ShmHandle *h)
{
    SECT("15. Hash 타입 불일치");
    s_replyObject *r;
    run(h,"SET","kv_only","data",NULL);
    r = run(h,"HSET","kv_only","f","v",NULL);
    CHECK(is_err(r,SHM_ERR_KEY_EXISTS),"HSET on KV key →KEY_EXISTS"); reply_free(r);
    r = run(h,"GET","kv_only",NULL); CHECK(is_str(r,"data"),"KV 무결성 유지"); reply_free(r);
    run(h,"DEL","kv_only",NULL);
}

/* ─────────────────────────────────────────────────────────
 *  16. Hash 대량 field
 * ───────────────────────────────────────────────────────── */
#define HBULK 500
static void test_hash_bulk(ShmHandle *h)
{
    SECT("16. Hash 대량 field (500개)");
    s_replyObject *r;
    run(h,"HCREATE","bulk_h",NULL);

    int fadd=0, fget=0, fdel=0;
    for(int i=0;i<HBULK;i++){
        char fi[32],vi[32];
        snprintf(fi,sizeof(fi),"field_%04d",i);
        snprintf(vi,sizeof(vi),"value_%04d",i);
        r = run(h,"HSET","bulk_h",fi,vi,NULL);
        if(!r||r->type==REPLY_ERROR) fadd++;
        reply_free(r);
    }
    CHECK(fadd==0,"대량 HSET 전부 성공");

    r = run(h,"HLEN","bulk_h",NULL); CHECK(is_int(r,HBULK),"HLEN==HBULK"); reply_free(r);

    for(int i=0;i<HBULK;i++){
        char fi[32],vi[32];
        snprintf(fi,sizeof(fi),"field_%04d",i);
        snprintf(vi,sizeof(vi),"value_%04d",i);
        r = run(h,"HGET","bulk_h",fi,NULL);
        if(!is_str(r,vi)) fget++;
        reply_free(r);
    }
    CHECK(fget==0,"대량 HGET 전부 정확");

    for(int i=0;i<HBULK/2;i++){
        char fi[32]; snprintf(fi,sizeof(fi),"field_%04d",i);
        r = run(h,"HDEL","bulk_h",fi,NULL);
        if(!is_int(r,1)) fdel++;
        reply_free(r);
    }
    CHECK(fdel==0,"절반 HDEL 성공");
    r = run(h,"HLEN","bulk_h",NULL); CHECK(is_int(r,HBULK-HBULK/2),"HLEN 절반 남음"); reply_free(r);

    run(h,"HDROP","bulk_h",NULL);
}

/* ─────────────────────────────────────────────────────────
 *  17. ZSet DROP 후 재생성
 * ───────────────────────────────────────────────────────── */
static void test_zdrop_recreate(ShmHandle *h)
{
    SECT("17. ZSet DROP 후 재생성");
    s_replyObject *r;
    run(h,"ZCREATE","tmp_z",NULL);
    run(h,"ZADD","tmp_z","1.0","x",NULL);
    run(h,"ZADD","tmp_z","2.0","y",NULL);

    r = run(h,"ZDROP","tmp_z",NULL); CHECK(is_ok(r),"ZDROP 성공"); reply_free(r);
    r = run(h,"ZCARD","tmp_z",NULL); CHECK(is_int(r,0),"ZCARD==0 after DROP"); reply_free(r);
    r = run(h,"ZDROP","tmp_z",NULL); CHECK(is_err(r,SHM_ERR_NOT_FOUND),"재DROP →NOT_FOUND"); reply_free(r);

    r = run(h,"ZCREATE","tmp_z",NULL); CHECK(is_ok(r),"재생성 OK"); reply_free(r);
    r = run(h,"ZADD","tmp_z","9.9","a",NULL); CHECK(is_int(r,1),"재생성 후 ZADD →1"); reply_free(r);
    run(h,"ZDROP","tmp_z",NULL);
}

/* ─────────────────────────────────────────────────────────
 *  18. cmd_table_get – 커맨드 테이블
 * ───────────────────────────────────────────────────────── */
static void test_cmd_table(ShmHandle *h)
{
    SECT("18. cmd_table_get");
    (void)h;
    size_t cnt = 0;
    const CmdEntry *tbl = cmd_table_get(&cnt);
    CHECK(cnt >= 28, "커맨드 수 >= 28");
    printf("  등록된 커맨드 (%zu개):\n", cnt);
    for (size_t i = 0; i < cnt; i++)
        printf("    %-18s  %s\n", tbl[i].name, tbl[i].usage);

    /* 알 수 없는 커맨드 */
    s_replyObject *r = run(h,"UNKNOWN_CMD","arg",NULL);
    CHECK(r&&r->type==REPLY_ERROR,"알 수 없는 커맨드 →ERROR"); reply_free(r);
}

/* ─────────────────────────────────────────────────────────
 *  19. reply_print 시각 확인
 * ───────────────────────────────────────────────────────── */
static void test_reply_print(ShmHandle *h)
{
    SECT("19. reply_print 출력 검증");
    s_replyObject *r;

    run(h,"HCREATE","demo",NULL);
    run(h,"HSET","demo","name","Claude","version","5","type","AI",NULL);
    r = run(h,"HGETALL","demo",NULL);
    printf("  HGETALL demo:\n"); reply_print(r, 4);
    CHECK(r&&r->type==REPLY_ARRAY,"HGETALL 반환 타입 ARRAY"); reply_free(r);

    run(h,"ZCREATE","demo_z",NULL);
    run(h,"ZADD","demo_z","3.14","pi",NULL);
    run(h,"ZADD","demo_z","2.72","e",NULL);
    r = run(h,"ZRANGE","demo_z","0","-1",NULL);
    printf("  ZRANGE demo_z:\n"); reply_print(r, 4);
    CHECK(r&&r->type==REPLY_ARRAY,"ZRANGE 반환 타입 ARRAY"); reply_free(r);

    run(h,"HDROP","demo",NULL);
    run(h,"ZDROP","demo_z",NULL);
}

/* ─────────────────────────────────────────────────────────
 *  20. 멀티프로세스 동시 SET + HSET + ZADD
 * ───────────────────────────────────────────────────────── */
#define MP_PROC  12
#define MP_KV    80
#define MP_HF    80
#define MP_ZA    80

static void test_multiproc(void)
{
    SECT("20. 멀티프로세스 동시 SET + HSET + ZADD");
    {
        ShmHandle *p = shm_open_existing(SHM_TEST);
        if (!p) { printf(FAIL " open 실패\n"); g_fail++; return; }
        run(p,"ZCREATE","mp_zs",NULL);
        run(p,"HCREATE","mp_hh",NULL);
        shm_close(p);
    }
    pid_t pids[MP_PROC];
    for (int p = 0; p < MP_PROC; p++) {
        pids[p] = fork();
        if (pids[p] == 0) {
            ShmHandle *ch = shm_open_existing(SHM_TEST);
            if (!ch) exit(1);
            shm_set_debug_level(DBG_ERROR);
            for (int i = 0; i < MP_KV; i++) {
                char k[32],v[32]; snprintf(k,sizeof(k),"kv_p%d_%03d",p,i); snprintf(v,sizeof(v),"val_p%d_%03d",p,i);
                s_replyObject *r = run(ch,"SET",k,v,NULL); reply_free(r);
            }
            for (int i = 0; i < MP_HF; i++) {
                char f[32],v[32]; snprintf(f,sizeof(f),"f_p%d_%03d",p,i); snprintf(v,sizeof(v),"v_p%d_%03d",p,i);
                s_replyObject *r = run(ch,"HSET","mp_hh",f,v,NULL); reply_free(r);
            }
            for (int i = 0; i < MP_ZA; i++) {
                char sc[32],m[32]; snprintf(sc,sizeof(sc),"%d",p*10000+i); snprintf(m,sizeof(m),"m_p%d_%03d",p,i);
                s_replyObject *r = run(ch,"ZADD","mp_zs",sc,m,NULL); reply_free(r);
            }
            shm_close(ch); exit(0);
        }
    }
    int all_ok = 1;
    for (int p = 0; p < MP_PROC; p++) {
        int st; waitpid(pids[p], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st)) all_ok = 0;
    }
    CHECK(all_ok, "모든 자식 정상 종료");

    ShmHandle *h = shm_open_existing(SHM_TEST);
    if (!h) { printf(FAIL " 검증 open 실패\n"); g_fail++; return; }
    shm_set_debug_level(DBG_ERROR);

    int mk=0, mh=0, mz=0;
    for (int p=0;p<MP_PROC;p++) for (int i=0;i<MP_KV;i++){
        char k[32],v[32]; snprintf(k,sizeof(k),"kv_p%d_%03d",p,i); snprintf(v,sizeof(v),"val_p%d_%03d",p,i);
        s_replyObject *r = run(h,"GET",k,NULL); if(!is_str(r,v)) mk++; reply_free(r);
    }
    CHECK(mk==0,"mp KV 모든 값 정확");

    s_replyObject *hl = run(h,"HLEN","mp_hh",NULL);
    CHECK(is_int(hl,(int64_t)(MP_PROC*MP_HF)),"mp HLEN==MP_PROC*MP_HF"); reply_free(hl);
    for (int p=0;p<MP_PROC;p++) for (int i=0;i<MP_HF;i++){
        char f[32],v[32]; snprintf(f,sizeof(f),"f_p%d_%03d",p,i); snprintf(v,sizeof(v),"v_p%d_%03d",p,i);
        s_replyObject *r = run(h,"HGET","mp_hh",f,NULL); if(!is_str(r,v)) mh++; reply_free(r);
    }
    CHECK(mh==0,"mp Hash 모든 field 정확");

    s_replyObject *zc = run(h,"ZCARD","mp_zs",NULL);
    CHECK(is_int(zc,(int64_t)(MP_PROC*MP_ZA)),"mp ZCARD==MP_PROC*MP_ZA"); reply_free(zc);
    for (int p=0;p<MP_PROC;p++) for (int i=0;i<MP_ZA;i++){
        char m[32]; snprintf(m,sizeof(m),"m_p%d_%03d",p,i);
        s_replyObject *r = run(h,"ZSCORE","mp_zs",m,NULL);
        if(!r||r->type==REPLY_NIL) mz++;
        else if(r->type==REPLY_STRING&&fabs(to_dbl(r)-(double)(p*10000+i))>1e-6) mz++;
        reply_free(r);
    }
    CHECK(mz==0,"mp ZSet 모든 score 정확");

    /* 정리 */
    run(h,"ZDROP","mp_zs",NULL); run(h,"HDROP","mp_hh",NULL);
    for(int p=0;p<MP_PROC;p++) for(int i=0;i<MP_KV;i++){
        char k[32]; snprintf(k,sizeof(k),"kv_p%d_%03d",p,i); run(h,"DEL",k,NULL);
    }
    shm_close(h);
}

/* ─────────────────────────────────────────────────────────
 *  main
 * ───────────────────────────────────────────────────────── */
int main(void)
{
    printf("==========================================\n");
    printf("  SHM v5  –  s_replyObject 통합 테스트\n");
    printf("==========================================\n");

    shm_set_debug_level(DBG_INFO);
    shm_destroy(SHM_TEST);

    ShmHandle *h = shm_create(SHM_TEST, 1ULL << 30);
    if (!h) { fprintf(stderr, "shm_create 실패\n"); return 1; }

    test_kv_basic(h);
    test_kv_update(h);
    test_kv_notfound(h);
    test_key_unique(h);
    test_zset_basic(h);
    test_zset_flags(h);
    test_zset_misc(h);
    test_zset_range(h);
    shm_dump_stats(h);
    test_hash_basic(h);
    test_hash_getall(h);
    test_hash_incr(h);
    test_hash_type_mismatch(h);
    shm_set_debug_level(DBG_ERROR);
    test_hash_bulk(h);
    shm_set_debug_level(DBG_INFO);
    test_zdrop_recreate(h);
    test_cmd_table(h);
    test_reply_print(h);

    shm_dump_stats(h);
    shm_close(h);

    test_multiproc();

    h = shm_open_existing(SHM_TEST);
    if (h) {
		shm_dump_stats(h);
		shm_close(h);
	}
    shm_destroy(SHM_TEST);

    printf("\n==========================================\n");
    printf("  최종: PASS=\033[32m%d\033[0m  FAIL=\033[31m%d\033[0m\n",
           g_pass, g_fail);
    printf("==========================================\n");
    return (g_fail == 0) ? 0 : 1;
}
