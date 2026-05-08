/*
 * resp_server.c  –  SHM v5 RESP Server (Redis 호환)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <pthread.h>

#include "shm_types.h"
#include "shm_core.h"
#include "cmd_pubsub.h"
#include "cmd_dispatch.h"

#define MAX_CLIENTS     1024
#define BUFFER_SIZE     16384
#define DEFAULT_PORT    6379
#define MAX_ARGS        128

static volatile int running = 1;
static ShmHandle *g_shm = NULL;

/* ============================================================
 *  SIGINT 처리
 * ============================================================ */
static void sigint_handler(int sig) {
    (void)sig;
    running = 0;
    printf("\n\n[INFO] 서버를 종료합니다...\n");
}

/* RESP 응답 직렬화 */
static void resp_send(int fd, s_replyObject *r)
{
    char tmp[1024];
    ssize_t n;

    if (!r) {
        write(fd, "$-1\r\n", 5);
        return;
    }

    switch (r->type) {
    case REPLY_STATUS:
        n = snprintf(tmp, sizeof(tmp), "+%s\r\n", (char*)r->ptr);
        write(fd, tmp, n);
        break;

    case REPLY_ERROR:
        n = snprintf(tmp, sizeof(tmp), "-%s\r\n", (char*)r->ptr);
        write(fd, tmp, n);
        break;

    case REPLY_INTEGER:
        n = snprintf(tmp, sizeof(tmp), ":%lld\r\n", (long long)r->integer);
        write(fd, tmp, n);
        break;

    case REPLY_STRING:
        n = snprintf(tmp, sizeof(tmp), "$%zu\r\n", r->len);
        write(fd, tmp, n);
        if (r->len > 0) write(fd, r->ptr, r->len);
        write(fd, "\r\n", 2);
        break;

    case REPLY_NIL:
        write(fd, "$-1\r\n", 5);
        break;

    case REPLY_ARRAY:
        n = snprintf(tmp, sizeof(tmp), "*%zu\r\n", r->elements);
        write(fd, tmp, n);
        for (size_t i = 0; i < r->elements; i++) {
            resp_send(fd, r->element[i]);   // 재귀 호출
        }
        break;

    default:
        write(fd, "-ERR unknown reply\r\n", 20);
    }
}
/* ============================================================
 *  명령어 처리 (PUB/SUB 지원)
 * ============================================================ */
static void process_command(int client_fd, ShmHandle *h, char *buf, size_t len)
{
    if (len == 0 || !h) return;
    buf[len] = '\0';

    string_t *args[MAX_ARGS] = {0};
    uint32_t argc = 0;

    // 간단 RESP 파싱 (실제로는 더 robust한 파서 권장)
    char *p = buf;
    if (*p == '*') {
        p++;
        uint32_t expected = (uint32_t)strtoul(p, &p, 10);
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;

        for (uint32_t i = 0; i < expected && argc < MAX_ARGS; i++) {
            if (*p == '$') {
                p++;
                size_t blen = strtoul(p, &p, 10);
                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;

                if (blen > 0) {
                    args[argc] = malloc(sizeof(string_t));
                    args[argc]->ptr = strndup(p, blen);
                    args[argc]->len = (uint32_t)blen;
                    argc++;
                    p += blen;
                    if (*p == '\r') p++;
                    if (*p == '\n') p++;
                }
            }
        }
    }

    if (argc == 0) {
        write(client_fd, "-ERR parse error\r\n", 18);
        goto cleanup;
    }

    /* 실제 커맨드 실행 */
    s_replyObject *reply = cmd_dispatch(h, args, argc);

    resp_send(client_fd, reply);
    reply_free(reply);

cleanup:
    for (uint32_t i = 0; i < argc; i++) {
        if (args[i]) {
            free((void*)args[i]->ptr);
            free(args[i]);
        }
    }
}

static void *client_handler(void *arg)
{
    int client_fd = (int)(intptr_t)arg;
    ShmHandle *h = g_shm;        // ← 전역 g_shm 사용
    char *buf = malloc(BUFFER_SIZE);
    ssize_t n;

    printf("[INFO] Client connected (fd=%d)\n", client_fd);

    while (running) {
        n = read(client_fd, buf, BUFFER_SIZE - 1);
        if (n <= 0) break;

        process_command(client_fd, h, buf, n);   // ← h 전달
    }

    printf("[INFO] Client disconnected (fd=%d)\n", client_fd);
    close(client_fd);
    free(buf);
    return NULL;
}

/* ============================================================
 *  MAIN
 * ============================================================ */
int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    if (argc > 1) port = atoi(argv[1]);

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    printf("==========================================\n");
    printf("     SHM v5 RESP Server (Redis Compatible)\n");
    printf("==========================================\n");

    // SHM 열기 또는 생성
    g_shm = shm_open_existing(SHM_DEFAULT_NAME);
    if (!g_shm) {
        printf("[INFO] SHM이 없어 새로 생성합니다... (1GB)\n");
        g_shm = shm_create(SHM_DEFAULT_NAME, 1ULL << 30);  // 1GB
    }

    if (!g_shm) {
        fprintf(stderr, "SHM 초기화 실패!\n");
        return 1;
    }

    shm_dump_stats(g_shm);

    // 서버 소켓 생성
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        perror("listen");
        return 1;
    }

    printf("✅ 서버 시작 완료! 포트: %d\n", port);
    printf("   redis-cli -p %d 로 접속해보세요.\n\n", port);

	struct timeval	tv;

    while (running) {
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		fd_set	fdset;
		FD_ZERO(&fdset);
		FD_SET(server_fd, &fdset);
		int	rc = select (server_fd + 1, &fdset, NULL, NULL, &tv);
		if (rc <= 0)	continue;

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) continue;

        pthread_t tid;
        pthread_create(&tid, NULL, client_handler, (void*)(intptr_t)client_fd);
        pthread_detach(tid);
    }

    close(server_fd);
    shm_close(g_shm);
    printf("서버 종료됨.\n");
    return 0;
}
