#ifndef CMD_PUBSUB_H
#define CMD_PUBSUB_H
/*
 * cmd_pubsub.h  –  SHM Pub/Sub (버그 수정본)
 *
 *  설계:
 *    - 채널 당 PublishHeader 1개 (SHM 힙)
 *    - PublishEntry 큐 (링크드 리스트, SHM 힙)
 *    - PID 배열 (SHM 힙, 동적 확장)
 *    - 시그널: SIGRTMIN+15, sigqueue(sival_ptr = entry offset)
 *    - 수신: signalfd + pubsub_handle_event()
 */
#include "mredis_types.h"
#include "mredis_core.h"

#define ENTRY_PUBSUB  0x7u
#define SIG_PUBSUB    (SIGRTMIN + 15u)
#define PUBSUB_PID_INIT_CNT  512

typedef struct PublishHeader {
    uint64_t        head_offset;      /* PublishEntry 큐 head  */
    uint64_t        tail_offset;      /* PublishEntry 큐 tail  */
    uint32_t        pidmaxcnt;        /* pid 배열 최대 용량    */
    uint32_t        pidcnt;           /* 등록된 pid 수         */
    uint32_t        channel_len;
    uint32_t        _pad;
    uint64_t        channel_offset;   /* 채널 이름 힙 오프셋   */
    uint64_t        pid_offset;       /* pid_t[] 힙 오프셋     */
    pthread_mutex_t mutex;            /* PROCESS_SHARED+ROBUST */
} PublishHeader;

typedef struct PublishEntry {
    uint64_t next_offset;
    uint32_t readcnt;     /* 남은 독자 수 (원자 감소)          */
    uint32_t len;         /* 메시지 바이트 수                  */
    uint64_t data_offset; /* 메시지 데이터 힙 오프셋           */
    uint64_t header_offset; /* 역참조: PublishHeader 오프셋   */
} PublishEntry;

typedef void (*PubSubCallback)(const char *channel, uint32_t clen,
                               const char *message,  uint32_t mlen,
                               void *user_data);

/* ── 커맨드 ─────────────────────────────────────────────── */
s_replyObject *cmd_publish    (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_subscribe  (MRedisHandle *h, string_t *args[], uint32_t argc);
s_replyObject *cmd_unsubscribe(MRedisHandle *h, string_t *args[], uint32_t argc);

/* ── 이벤트 수신 (signalfd read 후 호출) ─────────────────── */
s_replyObject *pubsub_handle_event(MRedisHandle *h, int signalfd_fd);

/* ── 유틸 ───────────────────────────────────────────────── */
void pubsub_set_callback(PubSubCallback cb, void *user_data);

/* 채널 헤더 검색/생성 (flag=0:검색만, flag=1:없으면생성) */
uint64_t find_or_create_channel_header(MRedisHandle *h, const char *channel,
                                        uint32_t clen, int flag);
void pubsub_cleanup(MRedisHandle *h);

/* stub – resp_server 링크 호환 */
static inline int  pubsub_start_listener(MRedisHandle *h) { (void)h; return 0; }
static inline void pubsub_stop_listener(void) {}

#endif /* CMD_PUBSUB_H */
