/*
 * cmd_pubsub.c  –  SHM Pub/Sub (버그 수정본)
 *
 * ── 수정 내역 ──────────────────────────────────────────────
 * [B1]  find_or_create: flag==0 이면 bucket unlock 후 반환
 * [B2]  find_or_create: channel/pid 할당 실패 시 ph_off 롤백
 * [B3]  delete_channel: ph->mutex 를 잠근 적 없는데 unlock 제거
 *        → 호출자(cmd_publish/unsubscribe)가 unlock 책임
 *        → delete 함수는 bucket_lock만 사용하는 독립 함수로 재설계
 * [B4]  delete_channel: ne 오프셋·데이터 오프셋을 지역변수로 복사 후
 *        bucket unlock → heap_free 순서로 변경
 * [B5]  cmd_publish: ph->mutex 해제 후 erase 여부 판단 → 별도 함수 호출
 * [B6]  cmd_publish: !entry_off → entry_off==OFFSET_NULL 로 수정
 * [B7]  cmd_publish: delete 호출 전 ph->mutex unlock 완료 보장
 * [B8]  sigval_t: memcpy 로 uint64_t ↔ sival_ptr 변환
 * [B9]  cmd_subscribe: 배열 확장 후 pids 포인터 재조회
 * [B10] cmd_subscribe: ph->pidcnt 를 mutex 구간 내에서만 읽기
 * [B11] cmd_unsubscribe: ph->mutex 해제 후 별도로 채널 삭제
 * [B12] cmd_unsubscribe: delete 호출 시 ph에 접근하지 않음
 * [B13] pubsub_handle_event: pe->readcnt 감소 시 ph 역참조 보호
 * [B14] pubsub_handle_event: head_offset 갱신을 ph->mutex 구간으로 이동
 * [B15] pubsub_cleanup: bucket_lock 사용, mutex 이중 잠금 제거
 * [B16] pidmaxcnt/pidcnt를 uint32_t 로 변경 (헤더에 반영)
 */
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include "shm_types.h"
#include "shm_core.h"
#include "cmd_pubsub.h"

static PubSubCallback g_callback  = NULL;
static void          *g_user_data = NULL;

/* ============================================================
 *  내부 헬퍼 – mutex 초기화 (PROCESS_SHARED + ROBUST)
 * ============================================================ */
static void ph_mutex_init(pthread_mutex_t *m)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setpshared(&a, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_setrobust(&a, PTHREAD_MUTEX_ROBUST);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}

static inline void ph_lock(PublishHeader *ph)
{
    int rc = pthread_mutex_lock(&ph->mutex);
    if (rc == EOWNERDEAD) pthread_mutex_consistent(&ph->mutex);
}

/* ============================================================
 *  §1  채널 헤더 찾기 / 생성
 *
 *  flag=0: 없으면 OFFSET_NULL 반환 (생성 안 함)
 *  flag=1: 없으면 새로 생성
 *
 * [B1] flag==0 일 때 bucket_lock 해제 후 반환
 * [B2] channel/pid 할당 실패 시 ph_off 포함 전체 롤백
 * ============================================================ */
uint64_t find_or_create_channel_header(ShmHandle *h,
                                               const char *channel,
                                               uint32_t    clen,
                                               int         flag)
{
    if (!h || clen == 0) return OFFSET_NULL;

    uint32_t     idx = shm_hash(channel, clen);
    BucketEntry *bk  = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t ne_off = bucket_find_locked(h, bk, channel, clen, ENTRY_PUBSUB, NULL);
    if (ne_off != OFFSET_NULL) {
        uint64_t ph_off = ((NameEntry *)OFF2PTR(h, ne_off))->data_offset;
        pthread_mutex_unlock(&bk->mutex);
        return ph_off;
    }

    /* [B1] flag==0 → unlock 후 반환 */
    if (flag == 0) {
        pthread_mutex_unlock(&bk->mutex);
        return OFFSET_NULL;
    }

    /* 새 PublishHeader 생성 */
    uint64_t ph_off = heap_alloc(h, sizeof(PublishHeader));
    if (ph_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return OFFSET_NULL;
    }
    PublishHeader *ph = (PublishHeader *)OFF2PTR(h, ph_off);
    memset(ph, 0, sizeof(PublishHeader));
    ph_mutex_init(&ph->mutex);
    ph->head_offset = OFFSET_NULL;
    ph->tail_offset = OFFSET_NULL;

    /* [B2] 부분 할당 실패 → 전체 롤백 */
    ph->channel_offset = heap_alloc(h, clen);
    ph->pid_offset     = heap_alloc(h, PUBSUB_PID_INIT_CNT * sizeof(pid_t));
    if (ph->channel_offset == OFFSET_NULL || ph->pid_offset == OFFSET_NULL) {
        if (ph->channel_offset != OFFSET_NULL) heap_free(h, ph->channel_offset);
        if (ph->pid_offset     != OFFSET_NULL) heap_free(h, ph->pid_offset);
        pthread_mutex_destroy(&ph->mutex);
        heap_free(h, ph_off);
        pthread_mutex_unlock(&bk->mutex);
        return OFFSET_NULL;
    }
    memcpy(OFF2PTR(h, ph->channel_offset), channel, clen);
    ph->channel_len = clen;
    ph->pidmaxcnt   = PUBSUB_PID_INIT_CNT;
    ph->pidcnt      = 0;
    memset(OFF2PTR(h, ph->pid_offset), 0, PUBSUB_PID_INIT_CNT * sizeof(pid_t));

    uint64_t new_ne = nameentry_alloc(h, channel, clen, ENTRY_PUBSUB, ph_off);
    if (new_ne == OFFSET_NULL) {
        heap_free(h, ph->pid_offset);
        heap_free(h, ph->channel_offset);
        pthread_mutex_destroy(&ph->mutex);
        heap_free(h, ph_off);
        pthread_mutex_unlock(&bk->mutex);
        return OFFSET_NULL;
    }
    ((NameEntry *)OFF2PTR(h, new_ne))->next_offset = bk->head_offset;
    bk->head_offset = new_ne;
    pthread_mutex_unlock(&bk->mutex);
    LOG_TRACE("PUBSUB: 채널 생성 '%.*s'", clen, channel);
    return ph_off;
}

/* ============================================================
 *  §2  채널 삭제 (호출 전 ph->mutex 미보유 상태여야 함)
 *
 * [B3]  ph->mutex 는 이 함수에서 잠그지 않음
 *        (PublishEntry 큐는 bucket_lock 구간 밖에서 해제)
 * [B4]  ne 포인터를 지역변수로 복사 후 bucket unlock
 * ============================================================ */
static void erase_channel(ShmHandle *h, const char *channel, uint32_t clen)
{
    if (!h || clen == 0) return;

    uint32_t     idx = shm_hash(channel, clen);
    BucketEntry *bk  = core_get_bucket(h, idx);
    bucket_lock(h, idx);

    uint64_t prev   = OFFSET_NULL;
    uint64_t ne_off = bucket_find_locked(h, bk, channel, clen, ENTRY_PUBSUB, &prev);
    if (ne_off == OFFSET_NULL) {
        pthread_mutex_unlock(&bk->mutex);
        return;
    }

    NameEntry  *ne     = (NameEntry *)OFF2PTR(h, ne_off);
    uint64_t    ph_off  = ne->data_offset;
    PublishHeader *ph   = (PublishHeader *)OFF2PTR(h, ph_off);

    /* 마지막으로 pidcnt 재확인 (bucket 잠긴 상태) */
    ph_lock(ph);
    if (ph->pidcnt > 0) {
        /* 다른 프로세스가 구독 → 삭제 취소 */
        pthread_mutex_unlock(&ph->mutex);
        pthread_mutex_unlock(&bk->mutex);
        LOG_TRACE("PUBSUB: 삭제 취소 (구독자 있음) '%.*s'", clen, channel);
        return;
    }
    pthread_mutex_unlock(&ph->mutex);

    /* [B4] 오프셋 지역변수 복사 → ne 접근 종료 */
    uint64_t data_off     = ph_off;           /* = ne->data_offset */
    uint64_t ch_off       = ph->channel_offset;
    uint64_t pid_off      = ph->pid_offset;
    uint64_t queue_head   = ph->head_offset;

    if (prev == OFFSET_NULL) bk->head_offset = ne->next_offset;
    else ((NameEntry *)OFF2PTR(h, prev))->next_offset = ne->next_offset;
    pthread_mutex_unlock(&bk->mutex);          /* bucket unlock 먼저 */

    /* PublishEntry 큐 해제 */
    uint64_t cur = queue_head;
    while (cur != OFFSET_NULL) {
        PublishEntry *pe = (PublishEntry *)OFF2PTR(h, cur);
        uint64_t nxt = pe->next_offset;
        heap_free(h, pe->data_offset);
        heap_free(h, cur);
        cur = nxt;
    }

    heap_free(h, pid_off);
    heap_free(h, ch_off);
    pthread_mutex_destroy(&ph->mutex);
    heap_free(h, data_off);           /* ph_off */
    nameentry_free(h, ne_off);
    LOG_TRACE("PUBSUB: 채널 삭제 완료 '%.*s'", clen, channel);
}

/* ============================================================
 *  §3  PUBLISH  channel  message
 *
 * [B5]  ph->mutex unlock 후 erase_channel 호출
 * [B6]  OFFSET_NULL 비교로 할당 실패 판별
 * [B7]  delete 전 ph->mutex 반드시 해제
 * [B8]  sival_ptr ↔ uint64_t: memcpy 사용
 * ============================================================ */
s_replyObject *cmd_publish(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 3)
        return reply_error(SHM_ERR_ARGC, "usage: PUBLISH channel message");

    const char *channel = args[1]->ptr; uint32_t clen = args[1]->len;
    const char *msg     = args[2]->ptr; uint32_t mlen = args[2]->len;

    /* 구독자 없으면 즉시 반환 */
    uint64_t ph_off = find_or_create_channel_header(h, channel, clen, 0);
    if (ph_off == OFFSET_NULL) return reply_integer(0);

    PublishHeader *ph = (PublishHeader *)OFF2PTR(h, ph_off);
    ph_lock(ph);

    /* 구독자 없으면 unlock 후 채널 삭제 */
    if (ph->pidcnt == 0) {
        pthread_mutex_unlock(&ph->mutex);       /* [B5] unlock 먼저 */
        erase_channel(h, channel, clen);
        return reply_integer(0);
    }

    /* [B6] OFFSET_NULL 로 실패 판별 */
    uint64_t entry_off = heap_alloc(h, sizeof(PublishEntry));
    uint64_t data_off  = heap_alloc(h, mlen > 0 ? mlen : 1);
    if (entry_off == OFFSET_NULL || data_off == OFFSET_NULL) {
        if (entry_off != OFFSET_NULL) heap_free(h, entry_off);
        if (data_off  != OFFSET_NULL) heap_free(h, data_off);
        pthread_mutex_unlock(&ph->mutex);
        return reply_integer(0);
    }

    if (mlen > 0) memcpy(OFF2PTR(h, data_off), msg, mlen);

    PublishEntry *pe = (PublishEntry *)OFF2PTR(h, entry_off);
    pe->next_offset   = OFFSET_NULL;
    pe->readcnt       = ph->pidcnt;
    pe->len           = mlen;
    pe->data_offset   = data_off;
    pe->header_offset = ph_off;

    if (ph->tail_offset == OFFSET_NULL)
        ph->head_offset = ph->tail_offset = entry_off;
    else {
        ((PublishEntry *)OFF2PTR(h, ph->tail_offset))->next_offset = entry_off;
        ph->tail_offset = entry_off;
    }

    /* sigqueue: [B8] memcpy로 uint64_t → sival_ptr */
    int delivered = 0;
    pid_t *pids = (pid_t *)OFF2PTR(h, ph->pid_offset);
    uint32_t pidcnt = ph->pidcnt;   /* 스냅샷 */

    for (uint32_t i = 0; i < pidcnt; i++) {
        if (pids[i] <= 0) continue;
        if (kill(pids[i], 0) != 0) {
            /* 죽은 PID → 슬롯 정리 */
            pids[i] = pids[ph->pidcnt - 1];
            pids[ph->pidcnt - 1] = 0;
            ph->pidcnt--;
            pidcnt--;
            i--;
            continue;
        }
        sigval_t val;
        memcpy(&val.sival_ptr, &entry_off, sizeof(uint64_t)); /* [B8] */
		LOG_INFO("sigqueue pid:%d", pids[i]);
        if (sigqueue(pids[i], SIG_PUBSUB, val) == 0)
            delivered++;
    }
    pthread_mutex_unlock(&ph->mutex);   /* [B7] unlock 먼저 */

    /* 전달 수신자가 0이면 채널 삭제 */
    if (ph->pidcnt == 0)
        erase_channel(h, channel, clen);   /* [B5] */

    return reply_integer(delivered);
}

/* ============================================================
 *  §4  SUBSCRIBE  channel [channel …]
 *
 * [B9]  배열 확장 후 pids 포인터 재조회
 * [B10] ph->pidcnt 를 mutex 구간 내에서만 참조
 * ============================================================ */
s_replyObject *cmd_subscribe(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2)
        return reply_error(SHM_ERR_ARGC, "usage: SUBSCRIBE channel [...]");

    s_replyObject *arr = reply_array((size_t)(argc - 1) * 3);
    if (!arr) return reply_error(SHM_ERR_NOMEM, "reply alloc failed");

    pid_t mypid = (pid_t)syscall(SYS_gettid);
	LOG_INFO("register pid:%d", mypid);
    for (uint32_t i = 1; i < argc; i++) {
        uint64_t ph_off = find_or_create_channel_header(
                            h, args[i]->ptr, args[i]->len, 1);
        if (ph_off == OFFSET_NULL) continue;

        PublishHeader *ph = (PublishHeader *)OFF2PTR(h, ph_off);
        ph_lock(ph);

        pid_t *pids = (pid_t *)OFF2PTR(h, ph->pid_offset);

        /* 중복 등록 확인 */
        int exists = 0;
        for (uint32_t j = 0; j < ph->pidcnt; j++) {
            if (pids[j] == mypid) { exists = 1; break; }
        }

        if (!exists) {
            /* [B9] 배열 확장 필요 시 */
            if (ph->pidcnt >= ph->pidmaxcnt) {
                uint32_t new_max    = ph->pidmaxcnt + PUBSUB_PID_INIT_CNT;
                uint64_t new_pid_off = heap_alloc(h, new_max * sizeof(pid_t));
                if (new_pid_off != OFFSET_NULL) {
                    memcpy(OFF2PTR(h, new_pid_off),
                           OFF2PTR(h, ph->pid_offset),
                           ph->pidcnt * sizeof(pid_t));
                    heap_free(h, ph->pid_offset);
                    ph->pid_offset = new_pid_off;
                    ph->pidmaxcnt  = new_max;
                    pids = (pid_t *)OFF2PTR(h, ph->pid_offset); /* [B9] 재조회 */
                }
            }
            if (ph->pidcnt < ph->pidmaxcnt) {
                pids[ph->pidcnt++] = mypid;
            }
        }

        /* [B10] mutex 구간 내에서 pidcnt 읽기 */
        int64_t cur_cnt = (int64_t)ph->pidcnt;
        pthread_mutex_unlock(&ph->mutex);

        reply_array_append(arr, reply_string("subscribe", 9));
        reply_array_append(arr, reply_string(args[i]->ptr, args[i]->len));
        reply_array_append(arr, reply_integer(cur_cnt));
    }
    return arr;
}

/* ============================================================
 *  §5  UNSUBSCRIBE  channel [channel …]
 *
 * [B11] ph->mutex 해제 후 erase_channel 호출 (이중 bucket_lock 방지)
 * [B12] erase 여부만 판단, 해제 후 ph 역참조 없음
 * ============================================================ */
s_replyObject *cmd_unsubscribe(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2)
        return reply_error(SHM_ERR_ARGC, "usage: UNSUBSCRIBE channel [...]");

    pid_t   mypid = (pid_t)syscall(SYS_gettid);
    int64_t count = 0;

    for (uint32_t i = 1; i < argc; i++) {
        const char *ch  = args[i]->ptr;
        uint32_t    clen = args[i]->len;

        uint64_t ph_off = find_or_create_channel_header(h, ch, clen, 0);
        if (ph_off == OFFSET_NULL) continue;

        PublishHeader *ph = (PublishHeader *)OFF2PTR(h, ph_off);
        ph_lock(ph);

        pid_t   *pids    = (pid_t *)OFF2PTR(h, ph->pid_offset);
        int      do_erase = 0;

        for (uint32_t j = 0; j < ph->pidcnt; j++) {
            if (pids[j] == mypid) {
                /* swap-and-shrink */
                pids[j] = pids[ph->pidcnt - 1];
                pids[ph->pidcnt - 1] = 0;
                ph->pidcnt--;
                if (ph->pidcnt == 0) do_erase = 1;
                count++;
                break;
            }
        }
        pthread_mutex_unlock(&ph->mutex);   /* [B11] unlock 먼저 */

        if (do_erase)
            erase_channel(h, ch, clen);     /* [B11] 별도 호출 */
    }
    return reply_integer(count);
}

/* ============================================================
 *  §6  pubsub_handle_event  –  signalfd 이벤트 처리
 *
 * [B8]  memcpy 로 ssi_ptr → entry_off 복원
 * [B13] pe->readcnt 감소 시 ph->mutex 보호
 * [B14] head_offset 갱신을 ph->mutex 구간 내 수행
 * ============================================================ */
s_replyObject *pubsub_handle_event(ShmHandle *h, int signalfd_fd)
{
    if (signalfd_fd < 0 || !h)
        return reply_error(SHM_ERR_INVAL, "invalid signalfd");

    struct signalfd_siginfo fdsi;
    ssize_t n = read(signalfd_fd, &fdsi, sizeof(fdsi));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return reply_nil();
        return reply_error(SHM_ERR, "signalfd read error");
    }
    if ((size_t)n != sizeof(fdsi) || fdsi.ssi_signo != (uint32_t)SIG_PUBSUB)
        return reply_nil();
    /* [B8] memcpy 로 uint64_t 복원 */
    uint64_t entry_off;
    memcpy(&entry_off, &fdsi.ssi_ptr, sizeof(uint64_t));
    if (entry_off == OFFSET_NULL) return reply_nil();
    if (entry_off == 0x00)	{
		// 정상적인 루트가 아니므로 NULL값 배출하자.
		return NULL;
	}

    PublishEntry  *pe = (PublishEntry  *)OFF2PTR(h, entry_off);
    if (pe->data_offset == OFFSET_NULL) return reply_nil();

    PublishHeader *ph = (PublishHeader *)OFF2PTR(h, pe->header_offset);

    /* 메시지 복사 (mutex 밖에서 읽되 data/header 오프셋은 불변) */
    uint32_t mlen = pe->len;
    char     *msg  = (char *)OFF2PTR(h, pe->data_offset);
    char     *ch   = (char *)OFF2PTR(h, ph->channel_offset);
    uint32_t  clen  = ph->channel_len;

    s_replyObject *reply = reply_array(3);
    if (!reply) return reply_error(SHM_ERR_NOMEM, "alloc 실패");
    reply_array_append(reply, reply_string("message", 7));
    reply_array_append(reply, reply_string(ch,  clen));
    reply_array_append(reply, reply_string(msg, mlen));

    if (g_callback) g_callback(ch, clen, msg, mlen, g_user_data);

    /* [B13][B14] readcnt 원자 감소 + head 갱신을 ph->mutex 보호 */
    ph_lock(ph);
    uint32_t remaining = __sync_sub_and_fetch(&pe->readcnt, 1);
    if (remaining == 0) {
        ph->head_offset = pe->next_offset;
        if (ph->head_offset == OFFSET_NULL)
            ph->tail_offset = OFFSET_NULL;
        pthread_mutex_unlock(&ph->mutex);
        heap_free(h, pe->data_offset);
        heap_free(h, entry_off);
    } else {
        pthread_mutex_unlock(&ph->mutex);
    }

    return reply;
}

/* ============================================================
 *  §7  pubsub_cleanup  –  모든 채널 해제
 *
 * [B15] bucket_lock 사용 + ph->mutex 이중 잠금 제거
 *        erase_channel 이 내부에서 bucket_lock 을 잡으므로
 *        cleanup 에서는 bucket_lock 없이 채널 이름을 수집 후 삭제
 * ============================================================ */
void pubsub_cleanup(ShmHandle *h)
{
    if (!h) return;
    LOG_TRACE("[PubSub] cleanup 시작");

    ShmHeader *shdr = core_shm_hdr(h);
    uint32_t cleaned = 0;

    for (uint32_t i = 0; i < shdr->hash_table_size; i++) {
        BucketEntry *bk = core_get_bucket(h, i);
        bucket_lock(h, i);

        uint64_t cur = bk->head_offset;
        while (cur != OFFSET_NULL) {
            NameEntry *ne  = (NameEntry *)OFF2PTR(h, cur);
            uint64_t   nxt = ne->next_offset;
            if (ne->type == ENTRY_PUBSUB && ne->data_offset != OFFSET_NULL) {
                PublishHeader *ph = (PublishHeader *)OFF2PTR(h, ne->data_offset);
                /* 살아있는 구독자 확인 */
                pid_t *pids = (pid_t *)OFF2PTR(h, ph->pid_offset);
                int alive = 0;
                for (uint32_t j = 0; j < ph->pidcnt; j++)
                    if (pids[j] > 0 && kill(pids[j], 0) == 0) { alive = 1; break; }

                if (!alive) {
                    /* 채널 이름 스택 복사 (bucket unlock 후 erase 호출용) */
                    uint32_t cl = ph->channel_len;
                    char     cbuf[256];
                    if (cl >= sizeof(cbuf)) cl = sizeof(cbuf) - 1;
                    memcpy(cbuf, OFF2PTR(h, ph->channel_offset), cl);
                    cbuf[cl] = '\0';

                    pthread_mutex_unlock(&bk->mutex);   /* [B15] bucket unlock 먼저 */
                    /* [B15] erase_channel 이 내부에서 bucket_lock 사용 */
                    ph_lock(ph);
                    ph->pidcnt = 0;
                    pthread_mutex_unlock(&ph->mutex);
                    erase_channel(h, cbuf, cl);
                    cleaned++;

                    /* 버킷이 변경됐으므로 재시작 */
                    bucket_lock(h, i);
                    cur = bk->head_offset;
                    continue;
                }
            }
            cur = nxt;
        }
        pthread_mutex_unlock(&bk->mutex);
    }
    LOG_TRACE("[PubSub] cleanup 완료: %u 채널 삭제", cleaned);
}

/* ============================================================
 *  §8  Callback 등록
 * ============================================================ */
void pubsub_set_callback(PubSubCallback cb, void *user_data)
{
    g_callback  = cb;
    g_user_data = user_data;
}
