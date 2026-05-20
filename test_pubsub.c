/*
 * test_pubsub.c  –  멀티프로세스 Pub/Sub 통합 테스트
 *
 *  테스트 항목:
 *   §01  SUBSCRIBE → PUBLISH → pubsub_handle_event 기본 흐름
 *   §02  멀티채널 SUBSCRIBE
 *   §03  UNSUBSCRIBE 후 메시지 미수신
 *   §04  구독자 없는 채널 PUBLISH → 0
 *   §05  멀티프로세스: N개 구독자, 1개 발행자 → 모두 수신
 *   §06  멀티프로세스: 동시 SUBSCRIBE 경쟁 → pidcnt 정확성
 *   §07  pubsub_cleanup → 죽은 채널 해제
 *   §08  대량 메시지 순서 보장
 *   §09  빈 메시지(len=0) 발행
 *   §10  채널 자동 삭제 (마지막 구독자 UNSUBSCRIBE)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <pthread.h>

#include "shm_types.h"
#include "shm_core.h"
#include "cmd_pubsub.h"

/* ─── 픽스처 ───────────────────────────────────────────── */
#define SHM_NAME   "/shm_pubsub_test"
#define SHM_SIZE   (128ULL * 1024 * 1024)

#define PASS  "\033[32m[PASS]\033[0m"
#define FAIL  "\033[31m[FAIL]\033[0m"
#define SECT(t) printf("\n\033[36m══ %s ══\033[0m\n",(t))

static int g_pass = 0, g_fail = 0;
#define CHECK(c,m) do{ \
    if(c){printf(PASS " %s\n",(m));g_pass++;} \
    else {printf(FAIL " %s  [L%d]\n",(m),__LINE__);g_fail++;} \
}while(0)

/* ─── cmd_dispatch 없이 직접 호출하는 래퍼 ─────────────── */
static s_replyObject *pub(ShmHandle *h, const char *ch, const char *msg)
{
    string_t a0={"PUBLISH",7}, a1={(char*)ch,(uint32_t)strlen(ch)};
    string_t a2={(char*)msg,(uint32_t)strlen(msg)};
    string_t *args[]={&a0,&a1,&a2};
    return cmd_publish(h, args, 3);
}
static s_replyObject *sub(ShmHandle *h, const char *ch)
{
    string_t a0={"SUBSCRIBE",9}, a1={(char*)ch,(uint32_t)strlen(ch)};
    string_t *args[]={&a0,&a1};
    return cmd_subscribe(h, args, 2);
}
static s_replyObject *unsub(ShmHandle *h, const char *ch)
{
    string_t a0={"UNSUBSCRIBE",11}, a1={(char*)ch,(uint32_t)strlen(ch)};
    string_t *args[]={&a0,&a1};
    return cmd_unsubscribe(h, args, 2);
}

/* ─── signalfd 생성 헬퍼 ───────────────────────────────── */
static int make_signalfd(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIG_PUBSUB);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    return signalfd(-1, &mask, SFD_CLOEXEC);
}

/* ─── signalfd 에서 메시지 1개 읽기 (타임아웃 ms) ──────── */
static s_replyObject *recv_msg(ShmHandle *h, int sfd, int timeout_ms)
{
    fd_set fds; FD_ZERO(&fds); FD_SET(sfd, &fds);
    struct timeval tv = { timeout_ms/1000, (timeout_ms%1000)*1000 };
    int r = select(sfd+1, &fds, NULL, NULL, &tv);
    if (r <= 0) return NULL;
    return pubsub_handle_event(h, sfd);
}

/* ============================================================
 *  §01  기본 흐름
 * ============================================================ */
static void t01_basic(ShmHandle *h)
{
    SECT("01. 기본: SUBSCRIBE → PUBLISH → 수신");

    int sfd = make_signalfd();
    s_replyObject *r;

    r = sub(h, "ch1");
    CHECK(r && r->type==REPLY_ARRAY && r->elements==3, "SUBSCRIBE 응답 배열");
    reply_free(r);

    r = pub(h, "ch1", "hello");
    CHECK(r && r->type==REPLY_INTEGER && r->integer==1, "PUBLISH → 1 delivered");
    reply_free(r);

    s_replyObject *msg = recv_msg(h, sfd, 500);
    CHECK(msg && msg->type==REPLY_ARRAY && msg->elements==3, "수신 ARRAY[3]");
    if (msg && msg->elements==3) {
        CHECK(strcmp((char*)msg->element[0]->ptr,"message")==0, "element[0]=message");
        CHECK(strcmp((char*)msg->element[1]->ptr,"ch1")==0,     "element[1]=ch1");
        CHECK(strcmp((char*)msg->element[2]->ptr,"hello")==0,   "element[2]=hello");
    }
    reply_free(msg);

    r = unsub(h, "ch1"); reply_free(r);
    close(sfd);
}

/* ============================================================
 *  §02  멀티채널 SUBSCRIBE
 * ============================================================ */
static void t02_multichannel(ShmHandle *h)
{
    SECT("02. 멀티채널 SUBSCRIBE");

    int sfd = make_signalfd();
    s_replyObject *r;

    /* 채널 3개 구독 */
    string_t a0={"SUBSCRIBE",9};
    string_t a1={"mc1",3}, a2={"mc2",3}, a3={"mc3",3};
    string_t *args[]={&a0,&a1,&a2,&a3};
    r = cmd_subscribe(h, args, 4);
    CHECK(r && r->type==REPLY_ARRAY && r->elements==9, "멀티 SUBSCRIBE 응답");
    reply_free(r);

    r = pub(h, "mc2", "msg2");
    CHECK(r && r->integer==1, "mc2 PUBLISH"); reply_free(r);

    s_replyObject *msg = recv_msg(h, sfd, 500);
    CHECK(msg && msg->elements==3 &&
          strcmp((char*)msg->element[1]->ptr,"mc2")==0, "mc2 수신");
    reply_free(msg);

    unsub(h,"mc1"); unsub(h,"mc2"); unsub(h,"mc3");
    close(sfd);
}

/* ============================================================
 *  §03  UNSUBSCRIBE 후 미수신
 * ============================================================ */
static void t03_unsubscribe(ShmHandle *h)
{
    SECT("03. UNSUBSCRIBE 후 메시지 미수신");

    int sfd = make_signalfd();
    s_replyObject *r;

    sub(h, "ch_unsub");
    r = unsub(h, "ch_unsub");
    CHECK(r && r->type==REPLY_INTEGER && r->integer==1, "UNSUBSCRIBE → 1");
    reply_free(r);

    r = pub(h, "ch_unsub", "should not arrive");
    CHECK(r && r->integer==0, "PUBLISH after unsub → 0"); reply_free(r);

    s_replyObject *msg = recv_msg(h, sfd, 200);
    CHECK(msg==NULL || msg->type==REPLY_NIL, "UNSUBSCRIBE 후 메시지 없음");
    reply_free(msg);
    close(sfd);
}

/* ============================================================
 *  §04  구독자 없는 채널
 * ============================================================ */
static void t04_no_subscriber(ShmHandle *h)
{
    SECT("04. 구독자 없는 채널 PUBLISH");

    s_replyObject *r = pub(h, "empty_ch", "nobody");
    CHECK(r && r->integer==0, "구독자 없음 → 0"); reply_free(r);
}

/* ============================================================
 *  §05  멀티프로세스: N 구독자, 1 발행자
 * ============================================================ */
#define MP_SUBS   4
#define MP_MSGS   10

static void subscriber_proc(const char *shm_name, int proc_idx __attribute__((unused)))
{
    ShmHandle *h = shm_open_existing(shm_name);
    if (!h) exit(1);

    int sfd = make_signalfd();
    s_replyObject *r = sub(h, "mp_ch");
    reply_free(r);

    /* 준비 완료 시그널 – 부모가 SIGUSR1 대기 */
    kill(getppid(), SIGUSR1);

    int recv_cnt = 0;
    for (int i = 0; i < MP_MSGS; i++) {
        s_replyObject *msg = recv_msg(h, sfd, 2000);
        if (msg && msg->type==REPLY_ARRAY && msg->elements==3)
            recv_cnt++;
        reply_free(msg);
    }

    unsub(h, "mp_ch");
    close(sfd);
    shm_close(h);
    exit(recv_cnt == MP_MSGS ? 0 : 1);
}

static void t05_multiprocess(ShmHandle *h)
{
    SECT("05. 멀티프로세스: N 구독자 × 1 발행자");

    /* SIGUSR1 카운터 */
    volatile int ready_cnt = 0;
    struct sigaction sa = {0};
    sa.sa_handler = (void(*)(int))SIG_IGN; /* 부모는 시그널 무시 */
    sigaction(SIGUSR1, &sa, NULL);

    pid_t pids[MP_SUBS];
    for (int i = 0; i < MP_SUBS; i++) {
        pids[i] = fork();
        if (pids[i] == 0)
            subscriber_proc(SHM_NAME, i);
    }

    /* 구독자 모두 준비될 때까지 대기 (최대 3초) */
    for (int w = 0; w < 30; w++) {
        usleep(100000);
        /* pidcnt 확인 */
        uint64_t ph_off = find_or_create_channel_header(h, "mp_ch", 5, 0);
        if (ph_off != OFFSET_NULL) {
            PublishHeader *ph = (PublishHeader *)OFF2PTR(h, ph_off);
            if (ph->pidcnt >= MP_SUBS) break;
        }
    }
    (void)ready_cnt;

    /* 메시지 발행 */
    for (int i = 0; i < MP_MSGS; i++) {
        char msg[32]; snprintf(msg, sizeof(msg), "msg_%d", i);
        s_replyObject *r = pub(h, "mp_ch", msg);
        if (r) CHECK(r->integer==MP_SUBS, "PUBLISH → MP_SUBS delivered");
        reply_free(r);
        usleep(10000);
    }

    /* 자식 완료 대기 */
    int all_ok = 1;
    for (int i = 0; i < MP_SUBS; i++) {
        int status = 0;
        waitpid(pids[i], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) all_ok = 0;
    }
    CHECK(all_ok, "모든 구독자 MP_MSGS 개 수신");
}

/* ============================================================
 *  §06  동시 SUBSCRIBE 경쟁 → pidcnt 정확성
 * ============================================================ */
#define RACE_PROCS  8

static void race_sub_proc(const char *shm_name)
{
    ShmHandle *h = shm_open_existing(shm_name);
    if (!h) exit(1);
    s_replyObject *r = sub(h, "race_ch");
    reply_free(r);
    shm_close(h);
    exit(0);
}

static void t06_subscribe_race(ShmHandle *h)
{
    SECT("06. 동시 SUBSCRIBE 경쟁 → pidcnt 정확성");
    pid_t pids[RACE_PROCS];
    for (int i = 0; i < RACE_PROCS; i++) {
        pids[i] = fork();
        if (pids[i] == 0) race_sub_proc(SHM_NAME);
    }
    for (int i = 0; i < RACE_PROCS; i++) waitpid(pids[i], NULL, 0);

    uint64_t ph_off = find_or_create_channel_header(h, "race_ch", 7, 0);
    CHECK(ph_off != OFFSET_NULL, "race_ch 채널 존재");
    if (ph_off != OFFSET_NULL) {
        PublishHeader *ph = (PublishHeader *)OFF2PTR(h, ph_off);
        CHECK((int)ph->pidcnt == RACE_PROCS, "pidcnt == RACE_PROCS");
    }
    /* 정리 */
    pubsub_cleanup(h);
}

/* ============================================================
 *  §07  pubsub_cleanup → 죽은 채널 해제
 * ============================================================ */
static void t07_cleanup(ShmHandle *h)
{
    SECT("07. pubsub_cleanup – 죽은 구독자 채널 해제");

    /* 자식이 구독 후 종료 (채널 유령) */
    pid_t pid = fork();
    if (pid == 0) {
        ShmHandle *ch = shm_open_existing(SHM_NAME);
        sub(ch, "ghost_ch");
        shm_close(ch);
        exit(0);  /* 구독 해제 없이 종료 */
    }
    waitpid(pid, NULL, 0);

    /* cleanup 전 채널 존재 확인 */
    uint64_t ph_off = find_or_create_channel_header(h, "ghost_ch", 8, 0);
    CHECK(ph_off != OFFSET_NULL, "cleanup 전 ghost_ch 존재");

    pubsub_cleanup(h);

    /* cleanup 후 채널 없어야 함 */
    ph_off = find_or_create_channel_header(h, "ghost_ch", 8, 0);
    CHECK(ph_off == OFFSET_NULL, "cleanup 후 ghost_ch 삭제됨");
}

/* ============================================================
 *  §08  대량 메시지 순서 보장
 * ============================================================ */
#define BULK_MSGS  50

static void t08_bulk_order(ShmHandle *h)
{
    SECT("08. 대량 메시지 순서 보장");

    int sfd = make_signalfd();
    sub(h, "bulk_ch");

    for (int i = 0; i < BULK_MSGS; i++) {
        char msg[16]; snprintf(msg, sizeof(msg), "%04d", i);
        s_replyObject *r = pub(h, "bulk_ch", msg);
        reply_free(r);
    }

    int ok = 1, prev = -1;
    for (int i = 0; i < BULK_MSGS; i++) {
        s_replyObject *msg = recv_msg(h, sfd, 500);
        if (!msg || msg->type!=REPLY_ARRAY || msg->elements<3) { ok=0; break; }
        int num = atoi((char*)msg->element[2]->ptr);
        if (num != prev+1) ok = 0;
        prev = num;
        reply_free(msg);
    }
    CHECK(ok, "대량 메시지 순서 정확 (0~49)");
    unsub(h, "bulk_ch");
    close(sfd);
}

/* ============================================================
 *  §09  빈 메시지 (len=0)
 * ============================================================ */
static void t09_empty_msg(ShmHandle *h)
{
    SECT("09. 빈 메시지(len=0) 발행/수신");

    int sfd = make_signalfd();
    sub(h, "empty_msg_ch");

    s_replyObject *r = pub(h, "empty_msg_ch", "");
    CHECK(r && r->integer==1, "빈 메시지 PUBLISH → 1"); reply_free(r);

    s_replyObject *msg = recv_msg(h, sfd, 500);
    CHECK(msg && msg->type==REPLY_ARRAY && msg->elements==3, "빈 메시지 수신 배열");
    if (msg && msg->elements==3)
        CHECK(msg->element[2]->len==0, "element[2] len=0");
    reply_free(msg);
    unsub(h, "empty_msg_ch");
    close(sfd);
}

/* ============================================================
 *  §10  채널 자동 삭제 (마지막 구독자 UNSUBSCRIBE)
 * ============================================================ */
static void t10_auto_delete(ShmHandle *h)
{
    SECT("10. 채널 자동 삭제 (마지막 구독자 UNSUBSCRIBE)");

    sub(h, "auto_del_ch");

    /* 채널 존재 확인 */
    uint64_t ph_off = find_or_create_channel_header(h, "auto_del_ch", 11, 0);
    CHECK(ph_off != OFFSET_NULL, "SUBSCRIBE 후 채널 존재");

    s_replyObject *r = unsub(h, "auto_del_ch");
    CHECK(r && r->integer==1, "UNSUBSCRIBE → 1"); reply_free(r);

    /* 채널 자동 삭제 확인 */
    ph_off = find_or_create_channel_header(h, "auto_del_ch", 11, 0);
    CHECK(ph_off == OFFSET_NULL, "마지막 구독자 해제 후 채널 자동 삭제");
}

/* ============================================================
 *  main
 * ============================================================ */
int main(void)
{
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║    Pub/Sub 멀티프로세스 통합 테스트 (10)     ║\n");
    printf("╚══════════════════════════════════════════════╝\n");

    shm_destroy(SHM_NAME);
    ShmHandle *h = shm_create(SHM_NAME, SHM_SIZE);
    if (!h) { fprintf(stderr, "SHM 생성 실패\n"); return 1; }
    shm_set_debug_level(DBG_ERROR);

    t01_basic(h);
    t02_multichannel(h);
    t03_unsubscribe(h);
    t04_no_subscriber(h);
    t05_multiprocess(h);
    t06_subscribe_race(h);
    t07_cleanup(h);
    t08_bulk_order(h);
    t09_empty_msg(h);
    t10_auto_delete(h);

	shm_dump_stats(h);
    shm_close(h);
    shm_destroy(SHM_NAME);

    printf("\n══════════════════════════════════════════════\n");
    printf("결과: \033[32mPASS %d\033[0m / \033[31mFAIL %d\033[0m / 합계 %d\n",
           g_pass, g_fail, g_pass + g_fail);
    printf("══════════════════════════════════════════════\n");
    return g_fail > 0 ? 1 : 0;
}
