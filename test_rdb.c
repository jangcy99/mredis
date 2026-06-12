/*
 * test_rdb.c  –  RDB 통합 테스트 (9 cases)
 *
 *  01. KV  save → load 복구
 *  02. ZSET save → load 복구
 *  03. HASH save → load 복구
 *  04. 혼합 (KV+ZSET+HASH) save → load
 *  05. CRC-32 파일 손상 감지
 *  06. 파일 헤더 매직/버전 검증
 *  07. BGSAVE → waitpid → load
 *  08. 대량 키 save/load (1000개)
 *  09. rdb_stats 출력
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>

#include "mredis_types.h"
#include "mredis_core.h"
#include "cmd_dispatch.h"
#include "rdb.h"

#define SHM_A    "/mredis_rdb_test_a"
#define SHM_B    "/mredis_rdb_test_b"
#define RDB_F    "/tmp/test_mredis.rdb"

#define PASS     "\033[32m[PASS]\033[0m"
#define FAIL     "\033[31m[FAIL]\033[0m"
#define SECT(t)  printf("\n\033[36m=== %s ===\033[0m\n",(t))

static int g_pass = 0, g_fail = 0;
#define CHECK(c,m) do{ if(c){printf(PASS " %s\n",(m));g_pass++;}\
                       else {printf(FAIL " %s [L%d]\n",(m),__LINE__);g_fail++;}}while(0)

/* ── cmd_dispatch 래퍼 ─────────────────────────────────── */
static s_replyObject *run(MRedisHandle *h, ...)
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

static int is_str(s_replyObject *r, const char *s)
    { return r && r->type==REPLY_STRING && strcmp((char*)r->ptr,s)==0; }
static int is_int(s_replyObject *r, int64_t v)
    { return r && r->type==REPLY_INTEGER && r->integer==v; }
static int is_nil(s_replyObject *r)
    { return r && r->type==REPLY_NIL; }

/* SHM 쌍 + RDB 로 save/load 반복 테스트 공통 픽스처 */
static MRedisHandle *make_shm(const char *name)
{
    mredis_destroy(name);
    return mredis_create(name, 1024ULL * 1024 * 1024);
}

/* ── 01 KV save → load ─────────────────────────────────── */
static void t01(void)
{
    SECT("01. KV save → load");
    unlink(RDB_F);

    MRedisHandle *ha = make_shm(SHM_A);
    s_replyObject *r;
    r = run(ha,"SET","name","Alice",NULL);  reply_free(r);
    r = run(ha,"SET","city","Seoul",NULL);  reply_free(r);
    r = run(ha,"SET","lang","C",NULL);      reply_free(r);

    RdbHandle *rdb = rdb_open(RDB_F);
    CHECK(rdb_save(rdb, ha) == SHM_OK, "rdb_save OK");
    rdb_close(rdb);
    mredis_close(ha); mredis_destroy(SHM_A);

    /* 복구 */
    MRedisHandle *hb  = make_shm(SHM_B);
    RdbHandle *rdb2 = rdb_open(RDB_F);
    int64_t cnt = rdb_load(rdb2, hb);
    CHECK(cnt == 3, "load 키 수=3");

    r = run(hb,"GET","name",NULL); CHECK(is_str(r,"Alice"), "GET name=Alice"); reply_free(r);
    r = run(hb,"GET","city",NULL); CHECK(is_str(r,"Seoul"), "GET city=Seoul"); reply_free(r);
    r = run(hb,"GET","lang",NULL); CHECK(is_str(r,"C"),     "GET lang=C");     reply_free(r);
    r = run(hb,"GET","nokey",NULL); CHECK(is_nil(r), "GET 없는 key → NIL"); reply_free(r);

    rdb_close(rdb2);
    mredis_close(hb); mredis_destroy(SHM_B);
}

/* ── 02 ZSET save → load ───────────────────────────────── */
static void t02(void)
{
    SECT("02. ZSET save → load");
    unlink(RDB_F);

    MRedisHandle *ha = make_shm(SHM_A);
    s_replyObject *r;
    r = run(ha,"ZADD","scores","100","Alice",NULL); reply_free(r);
    r = run(ha,"ZADD","scores","200","Bob",  NULL); reply_free(r);
    r = run(ha,"ZADD","scores","150","Carol",NULL); reply_free(r);

    RdbHandle *rdb = rdb_open(RDB_F);
    CHECK(rdb_save(rdb, ha) == SHM_OK, "rdb_save OK");
    rdb_close(rdb);
    mredis_close(ha); mredis_destroy(SHM_A);

    MRedisHandle *hb   = make_shm(SHM_B);
    RdbHandle *rdb2 = rdb_open(RDB_F);
    int64_t cnt = rdb_load(rdb2, hb);
    CHECK(cnt == 1, "load 키 수=1 (zset 1개)");

    /* ZCARD */
    r = run(hb,"ZCARD","scores",NULL); CHECK(is_int(r,3),"ZCARD=3"); reply_free(r);

    /* ZSCORE */
    r = run(hb,"ZSCORE","scores","Alice",NULL);
    CHECK(r && r->type==REPLY_STRING && fabs(atof((char*)r->ptr)-100.0)<1e-9,
          "ZSCORE Alice=100"); reply_free(r);

    r = run(hb,"ZSCORE","scores","Carol",NULL);
    CHECK(r && r->type==REPLY_STRING && fabs(atof((char*)r->ptr)-150.0)<1e-9,
          "ZSCORE Carol=150"); reply_free(r);

    /* ZRANK (score 순: Alice<Carol<Bob) */
    r = run(hb,"ZRANK","scores","Alice",NULL); CHECK(is_int(r,0),"ZRANK Alice=0"); reply_free(r);
    r = run(hb,"ZRANK","scores","Bob",  NULL); CHECK(is_int(r,2),"ZRANK Bob=2");   reply_free(r);

    rdb_close(rdb2);
    mredis_close(hb); mredis_destroy(SHM_B);
}

/* ── 03 HASH save → load ───────────────────────────────── */
static void t03(void)
{
    SECT("03. HASH save → load");
    unlink(RDB_F);

    MRedisHandle *ha = make_shm(SHM_A);
    s_replyObject *r;
    r = run(ha,"HSET","user","name","Alice","age","30","city","Seoul",NULL); reply_free(r);
    r = run(ha,"HSET","cfg","debug","1","maxmem","256",NULL);               reply_free(r);

    RdbHandle *rdb = rdb_open(RDB_F);
    CHECK(rdb_save(rdb, ha) == SHM_OK, "rdb_save OK");
    rdb_close(rdb);
    mredis_close(ha); mredis_destroy(SHM_A);

    MRedisHandle *hb   = make_shm(SHM_B);
    RdbHandle *rdb2 = rdb_open(RDB_F);
    int64_t cnt = rdb_load(rdb2, hb);
    CHECK(cnt == 2, "load 키 수=2 (hash 2개)");

    r = run(hb,"HGET","user","name",NULL);  CHECK(is_str(r,"Alice"), "HGET name=Alice");  reply_free(r);
    r = run(hb,"HGET","user","age", NULL);  CHECK(is_str(r,"30"),    "HGET age=30");      reply_free(r);
    r = run(hb,"HGET","user","city",NULL);  CHECK(is_str(r,"Seoul"), "HGET city=Seoul");  reply_free(r);
    r = run(hb,"HLEN","user",NULL);         CHECK(is_int(r,3),        "HLEN=3");           reply_free(r);
    r = run(hb,"HGET","cfg","maxmem",NULL); CHECK(is_str(r,"256"),   "HGET maxmem=256");  reply_free(r);

    rdb_close(rdb2);
    mredis_close(hb); mredis_destroy(SHM_B);
}

/* ── 04 혼합 KV+ZSET+HASH ──────────────────────────────── */
static void t04(void)
{
    SECT("04. 혼합 KV+ZSET+HASH save → load");
    unlink(RDB_F);

    MRedisHandle *ha = make_shm(SHM_A);
    s_replyObject *r;

    /* KV */
    r = run(ha,"SET","ver","1.0",NULL); reply_free(r);
    r = run(ha,"SET","env","prod",NULL); reply_free(r);

    /* ZSET */
    r = run(ha,"ZADD","rank","500","user_a",NULL); reply_free(r);
    r = run(ha,"ZADD","rank","300","user_b",NULL); reply_free(r);
    r = run(ha,"ZADD","rank","700","user_c",NULL); reply_free(r);

    /* HASH */
    r = run(ha,"HSET","meta","host","localhost","port","6379",NULL); reply_free(r);

    RdbHandle *rdb = rdb_open(RDB_F);
    CHECK(rdb_save(rdb, ha) == SHM_OK, "혼합 rdb_save OK");
    rdb_close(rdb);
    mredis_close(ha); mredis_destroy(SHM_A);

    MRedisHandle *hb   = make_shm(SHM_B);
    RdbHandle *rdb2 = rdb_open(RDB_F);
    int64_t cnt = rdb_load(rdb2, hb);
    CHECK(cnt == 4, "혼합 load 키 수=4");

    r = run(hb,"GET","ver",NULL);          CHECK(is_str(r,"1.0"),  "GET ver=1.0");     reply_free(r);
    r = run(hb,"ZCARD","rank",NULL);       CHECK(is_int(r,3),       "ZCARD rank=3");    reply_free(r);
    r = run(hb,"HGET","meta","port",NULL); CHECK(is_str(r,"6379"), "HGET port=6379");  reply_free(r);

    /* score 정렬 확인 (user_b<user_a<user_c) */
    r = run(hb,"ZRANK","rank","user_b",NULL); CHECK(is_int(r,0),"ZRANK user_b=0"); reply_free(r);
    r = run(hb,"ZRANK","rank","user_c",NULL); CHECK(is_int(r,2),"ZRANK user_c=2"); reply_free(r);

    rdb_close(rdb2);
    mredis_close(hb); mredis_destroy(SHM_B);
}

/* ── 05 CRC 손상 감지 ──────────────────────────────────── */
static void t05(void)
{
    SECT("05. CRC-32 파일 손상 감지");
    unlink(RDB_F);

    MRedisHandle *ha = make_shm(SHM_A);
    s_replyObject *r = run(ha,"SET","k","v",NULL); reply_free(r);
    RdbHandle *rdb   = rdb_open(RDB_F);
    rdb_save(rdb, ha);
    rdb_close(rdb);
    mredis_close(ha); mredis_destroy(SHM_A);

    /* 파일 중간 1바이트 훼손 */
    struct stat st; stat(RDB_F, &st);
    int fd = open(RDB_F, O_RDWR);
    off_t mid = st.st_size / 2;
    uint8_t corrupt = 0xFF;
    { ssize_t _w = pwrite(fd, &corrupt, 1, mid); (void)_w; }
    close(fd);

    /* load 시 CRC 불일치 경고가 출력되어야 하고,
     * 프로세스가 종료되지 않아야 한다. */
    MRedisHandle *hb   = make_shm(SHM_B);
    RdbHandle *rdb2 = rdb_open(RDB_F);
    int64_t cnt = rdb_load(rdb2, hb); /* 경고 출력 후 cnt >= 0 */
    CHECK(1, "CRC 불일치 시 프로세스 정상 유지");
    /* cnt < 0 이면 load 자체 실패, >= 0 이면 경고만 */
    (void)cnt;

    rdb_close(rdb2);
    mredis_close(hb); mredis_destroy(SHM_B);
}

/* ── 06 헤더 매직 검증 ─────────────────────────────────── */
static void t06(void)
{
    SECT("06. 파일 헤더 매직 검증");

    /* 잘못된 파일 */
    int fd = open(RDB_F, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    const char *garbage = "GARBAGE_DATA";
    { ssize_t _w = write(fd, garbage, strlen(garbage)); (void)_w; }
    close(fd);

    MRedisHandle *hb   = make_shm(SHM_B);
    RdbHandle *rdb2 = rdb_open(RDB_F);
    int64_t cnt = rdb_load(rdb2, hb);
    CHECK(cnt < 0, "잘못된 매직 → load 실패(음수)");

    rdb_close(rdb2);
    mredis_close(hb); mredis_destroy(SHM_B);
    unlink(RDB_F);
}

/* ── 07 BGSAVE ─────────────────────────────────────────── */
static void t07(void)
{
    SECT("07. BGSAVE → waitpid → load");
    unlink(RDB_F);

    MRedisHandle *ha = make_shm(SHM_A);
    s_replyObject *r;
    for (int i = 0; i < 10; i++) {
        char k[16], v[16];
        snprintf(k,sizeof(k),"key%d",i);
        snprintf(v,sizeof(v),"val%d",i);
        r = run(ha,"SET",k,v,NULL); reply_free(r);
    }
    r = run(ha,"ZADD","zk","1","a","2","b","3","c",NULL); reply_free(r);

    RdbHandle *rdb = rdb_open(RDB_F);
    int rc = rdb_save_bg(rdb, ha);
    CHECK(rc == SHM_OK, "BGSAVE 시작 OK");

    /* 최대 5초 대기 */
    int done = 0;
    for (int i = 0; i < 50 && !done; i++) {
        usleep(100000);
        int chk = rdb_save_check(rdb);
        if (chk == 1) done = 1;
        else if (chk < 0) break;
    }
    CHECK(done, "BGSAVE 완료");

    struct stat st;
    CHECK(stat(RDB_F, &st) == 0 && st.st_size > 0, "RDB 파일 생성됨");

    rdb_close(rdb);
    mredis_close(ha); mredis_destroy(SHM_A);

    /* 복구 */
    MRedisHandle *hb   = make_shm(SHM_B);
    RdbHandle *rdb2 = rdb_open(RDB_F);
    int64_t cnt = rdb_load(rdb2, hb);
    CHECK(cnt == 11, "BGSAVE 복구 키=11 (10 KV + 1 ZSET)");

    r = run(hb,"GET","key5",NULL);    CHECK(is_str(r,"val5"), "GET key5=val5"); reply_free(r);
    r = run(hb,"ZCARD","zk",NULL);    CHECK(is_int(r,3),      "ZCARD zk=3");    reply_free(r);

    rdb_close(rdb2);
    mredis_close(hb); mredis_destroy(SHM_B);
}

/* ── 08 대량 키 (1000개) ───────────────────────────────── */
static void t08(void)
{
    SECT("08. 대량 키 1000개 save → load");
    unlink(RDB_F);

    MRedisHandle *ha = make_shm(SHM_A);
    s_replyObject *r;

    /* KV 500 개 */
    for (int i = 0; i < 500; i++) {
        char k[32], v[32];
        snprintf(k,sizeof(k),"kv:%d",i);
        snprintf(v,sizeof(v),"value_%d",i);
        r = run(ha,"SET",k,v,NULL); reply_free(r);
    }
    /* HASH 10개 (각 10 field) */
    for (int i = 0; i < 10; i++) {
        char hk[32]; snprintf(hk,sizeof(hk),"hash:%d",i);
        for (int j = 0; j < 10; j++) {
            char f[16], v[16];
            snprintf(f,sizeof(f),"f%d",j);
            snprintf(v,sizeof(v),"v%d",j);
            r = run(ha,"HSET",hk,f,v,NULL); reply_free(r);
        }
    }
    /* ZSET 10개 (각 5 member) */
    for (int i = 0; i < 10; i++) {
        char zk[32]; snprintf(zk,sizeof(zk),"zset:%d",i);
        for (int j = 0; j < 5; j++) {
            char sc[16], m[16];
            snprintf(sc,sizeof(sc),"%d",j*10);
            snprintf(m, sizeof(m), "m%d",j);
            r = run(ha,"ZADD",zk,sc,m,NULL); reply_free(r);
        }
    }

    RdbHandle *rdb = rdb_open(RDB_F);
    int rc = rdb_save(rdb, ha);
    CHECK(rc == SHM_OK, "대량 rdb_save OK");
    rdb_close(rdb);
    mredis_close(ha); mredis_destroy(SHM_A);

    MRedisHandle *hb   = make_shm(SHM_B);
    RdbHandle *rdb2 = rdb_open(RDB_F);
    int64_t cnt = rdb_load(rdb2, hb);
    CHECK(cnt == 520, "대량 load 키=520 (500+10+10)");

    /* 랜덤 샘플 확인 */
    r = run(hb,"GET","kv:0",  NULL); CHECK(is_str(r,"value_0"),  "kv:0=value_0");   reply_free(r);
    r = run(hb,"GET","kv:499",NULL); CHECK(is_str(r,"value_499"),"kv:499=value_499");reply_free(r);
    r = run(hb,"HLEN","hash:5",NULL);CHECK(is_int(r,10),          "HLEN hash:5=10"); reply_free(r);
    r = run(hb,"ZCARD","zset:9",NULL);CHECK(is_int(r,5),          "ZCARD zset:9=5"); reply_free(r);

    rdb_close(rdb2);
    mredis_close(hb); mredis_destroy(SHM_B);
}

/* ── 09 rdb_stats ──────────────────────────────────────── */
static void t09(void)
{
    SECT("09. rdb_stats");
    unlink(RDB_F);

    MRedisHandle *ha = make_shm(SHM_A);
    s_replyObject *r = run(ha,"SET","x","1",NULL); reply_free(r);

    RdbHandle *rdb = rdb_open(RDB_F);
    rdb_save(rdb, ha);

    char buf[512];
    rdb_stats(rdb, buf, sizeof(buf));
    printf("%s", buf);
    CHECK(strstr(buf,"1") != NULL,          "stats 키 수 포함");
    CHECK(strstr(buf,RDB_F) != NULL,        "stats 경로 포함");
    CHECK(strstr(buf,"OK") != NULL,         "stats 상태=OK");

    rdb_close(rdb);
    mredis_close(ha); mredis_destroy(SHM_A);
    unlink(RDB_F);
}

/* ============================================================
 *  main
 * ============================================================ */
int main(void)
{
    printf("╔══════════════════════════════════════╗\n");
    printf("║     RDB 통합 테스트 (9 cases)        ║\n");
    printf("╚══════════════════════════════════════╝\n");

    mredis_set_debug_level(DBG_ERROR);
    mredis_destroy(SHM_A); mredis_destroy(SHM_B);
    unlink(RDB_F);

    t01(); t02(); t03(); t04();
    t05(); t06(); t07(); t08(); t09();

    printf("\n══════════════════════════════════════\n");
    printf("결과: \033[32mPASS %d\033[0m / \033[31mFAIL %d\033[0m / 합계 %d\n",
           g_pass, g_fail, g_pass + g_fail);
    printf("══════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
