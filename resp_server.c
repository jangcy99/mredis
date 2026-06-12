/*
 * resp_server.c – Robust RESP Parser + Pub/Sub 지원
 */
#include <stdio.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <ctype.h>
#include <libgen.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mredis_types.h"
#include "mredis_core.h"
#include "cmd_pubsub.h"
#include "cmd_dispatch.h"

#define MAX_CLIENTS     1024
#define BUFFER_SIZE     32768
#define DEFAULT_PORT    6379
#define MAX_ARGS        256

pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static volatile int running = 1;
static MRedisHandle *g_shm = NULL;
static int	sigfd;

static void sigint_handler(int sig) {
    (void)sig;
    running = 0;
    printf("\n\n[INFO] 서버를 종료합니다...\n");
}


/* ============================================================
 *  Robust RESP Parser State
 * ============================================================ */
typedef struct {
    char    *buffer;
    size_t   capacity;
    size_t   len;
    size_t   pos;
} RespParser;

static RespParser* resp_parser_new(void)
{
    RespParser *p = malloc(sizeof(RespParser));
    p->capacity = BUFFER_SIZE;
    p->buffer = malloc(p->capacity);
    p->len = 0;
    p->pos = 0;
    return p;
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
static void resp_parser_free(RespParser *p)
{
    if (p) {
        free(p->buffer);
        free(p);
    }
}

static void resp_parser_append(RespParser *p, const char *data, size_t len)
{
    if (p->len + len > p->capacity) {
        p->capacity = p->len + len + BUFFER_SIZE;
        p->buffer = realloc(p->buffer, p->capacity);
    }
    memcpy(p->buffer + p->len, data, len);
    p->len += len;
}

/* ============================================================
 *  RESP 파싱 (Robust)
 * ============================================================ */
static int parse_resp(RespParser *p, string_t *args[], uint32_t *argc_out)
{
    *argc_out = 0;
    char *line;

    if (p->len == 0) return 0;

    // 첫 번째 줄 읽기
    char *end = memchr(p->buffer, '\n', p->len);
    if (!end) return 0;  // 불완전

    *end = '\0';
    if (end > p->buffer && *(end-1) == '\r') *(end-1) = '\0';

    line = p->buffer;

    if (line[0] != '*') {
        // Simple protocol fallback
        p->pos = end - p->buffer + 1;
        return 0;
    }

    uint32_t argc = (uint32_t)strtoul(line + 1, NULL, 10);
    if (argc == 0 || argc > MAX_ARGS) {
        p->pos = end - p->buffer + 1;
        return 0;
    }

    p->pos = end - p->buffer + 1;

    for (uint32_t i = 0; i < argc; i++) {
        // $len\r\n
        end = memchr(p->buffer + p->pos, '\n', p->len - p->pos);
        if (!end) return 0;

        *end = '\0';
        if (*(end-1) == '\r') *(end-1) = '\0';

        size_t bulk_len = strtoul(p->buffer + p->pos + 1, NULL, 10);
        p->pos = end - p->buffer + 1;

        if (bulk_len == 0) {
            args[i] = malloc(sizeof(string_t));
            args[i]->ptr = strdup("");
            args[i]->len = 0;
            continue;
        }

        if (p->pos + bulk_len >= p->len) return 0; // 불완전

        args[i] = malloc(sizeof(string_t));
        args[i]->ptr = strndup(p->buffer + p->pos, bulk_len);
        args[i]->len = bulk_len;

        p->pos += bulk_len + 2;  // \r\n
    }

    *argc_out = argc;
    return 1;
}

static int make_signalfd(void)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIG_PUBSUB);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    return signalfd(-1, &mask, SFD_CLOEXEC);
}

typedef struct	{
	pthread_mutex_t	*mutex;
	int		sockfd;
} client_info_t;

typedef struct	{
	char	*channel;
	int		refcnt;
	int		fdcnt;
	client_info_t	client[1024];
	void	*next;
} channel_info_t;

channel_info_t	*chnnl;

void *subscribe_cleaner (void *args __attribute__((unused)))	{
	while (running)	{
		sleep (3);	// 그냥 3초마다 돌면서 pidcnt가 0인 넘을 찾아서 지우자
		pubsub_cleanup(g_shm);
	}
	printf ("%s Terminated", __func__);
	return NULL;
}

void *subscribe_manager (void *args __attribute__((unused)))	{
	while (running)	{
		fd_set	fdset;
		FD_ZERO(&fdset);
		FD_SET (sigfd, &fdset);
		int rc = select (sigfd + 1, &fdset, NULL, NULL, NULL);
		if (rc <= 0)	continue;
		s_replyObject *r = pubsub_handle_event(g_shm, sigfd);
		if (r == NULL)	continue;
		if (r->type == REPLY_NIL)	continue;
		if (r->type != REPLY_ARRAY)	{
			printf ("subscribe reply warning\n");
			reply_print (r, 0);
			continue;
		}
		channel_info_t	*c = chnnl;
		while (c)	{
			if (memcmp (c->channel, r->element[1]->ptr, r->element[1]->len) != 0)	{
				c = c->next;
				continue;
			}
			for (int i=0;i<c->fdcnt;i++)	{
				if (c->client[i].sockfd < 0)	continue;
				pthread_mutex_lock (c->client[i].mutex);
				resp_send(c->client[i].sockfd, r);
				pthread_mutex_unlock (c->client[i].mutex);

			}
			break;
		}
		reply_free(r);
	}
	close (sigfd);
	printf ("%s Terminated", __func__);
	return NULL;
}
int register_subscribe (int fd, pthread_mutex_t *mutex, const void *ptr, int len)	{
	char	channel[1024];
	sprintf (channel, "%.*s", len, (char*)ptr);
	pthread_mutex_lock (&g_mutex);
	channel_info_t	*c = chnnl;
	while (c)	{
		if (strcmp (channel, c->channel) == 0)	{
			// 같은채널이 있는지 체크해야한다.
			int	existflag = 0;
			for (int i=0;i<c->fdcnt;i++)	{
				if (c->client[i].sockfd == fd)	{
					// 중복채널
					existflag = 1;
					break;
				}
			}
			if (!existflag)	{
				if (c->fdcnt == 0)	{
					c->client[0].sockfd = fd;
					c->client[0].mutex = mutex;
					break;
				}
				c->client[c->fdcnt].sockfd = fd;
				c->client[c->fdcnt].mutex = mutex;
				c->fdcnt ++;
			}
			break;
		}
		c = c->next;
	}
	if (!c)	{	// new channel register
		c = malloc (sizeof (*c));
		memset (c, 0x00, sizeof (*c));
		c->channel = strdup (channel);
		c->client[0].sockfd = fd;
		c->client[0].mutex = mutex;
		c->fdcnt = 1;
		c->next = NULL;
		if (!chnnl)	chnnl = c;
		else	{
			c->next = chnnl;
			chnnl = c;
		}
	}
	pthread_mutex_unlock (&g_mutex);
	return 0;
}

int unregister_subscribe (int sockfd, string_t *args)	{
	char	channel[128];
	sprintf (channel, "%.*s", args->len, args->ptr);
	pthread_mutex_lock (&g_mutex);
	channel_info_t *c = chnnl;
	channel_info_t *prev = NULL;
	while (c)	{
		if (strcmp (channel, c->channel) != 0)	{
			prev = c;
			c = c->next;
			continue;
		}
		for (int i=0;i<c->fdcnt;i++)	{
			if (c->client[i].sockfd == sockfd)	{
				if (c->fdcnt <= 1)	{
					// delete channel
					free (c->channel);
					if (c == chnnl)	chnnl = c->next;
					else if (prev)	prev->next = c->next;
					free (c);
				}
				else	{
					c->fdcnt --;
					c->client[i].sockfd = c->client[c->fdcnt - 1].sockfd;
					c->client[i].mutex = c->client[c->fdcnt - 1].mutex;
				}
				break;
			}
		}
		break;
	}
	pthread_mutex_unlock (&g_mutex);
	return 0;
}

void fd_clear_subscribe (int sockfd)	{
	pthread_mutex_lock (&g_mutex);
	channel_info_t *c = chnnl;
	channel_info_t *prev = NULL;
	while (c)	{
		for (int i=0;i<c->fdcnt;i++)	{
			if (c->client[i].sockfd == sockfd)	{
				if (c->fdcnt <= 1)	{
					// delete channel
					free (c->channel);
					if (c == chnnl)	{
						prev = NULL;
						chnnl = c->next;
						if (chnnl == NULL)	break;
					}
					else if (prev)	prev->next = c->next;
					void *next = c->next;
					free (c);
					c = next;
					break;
				}
				else	{
					c->fdcnt --;
					c->client[i].sockfd = c->client[c->fdcnt - 1].sockfd;
					c->client[i].mutex = c->client[c->fdcnt - 1].mutex;
				}
				break;
			}
		}
		if (chnnl == NULL)	break;
		prev = c;
		c = c->next;
	}
	pthread_mutex_unlock (&g_mutex);
	return;
}
/* ============================================================
 *  명령어 처리
 * ============================================================ */
static void process_command(pthread_mutex_t *mutex, int client_fd, MRedisHandle *h, RespParser *parser)
{
    string_t *args[MAX_ARGS] = {0};
    uint32_t argc = 0;

    if (!parse_resp(parser, args, &argc) || argc == 0) return;

	s_replyObject *reply = NULL;
	reply = cmd_dispatch(h, args, argc);
	if (reply)	{
		// subscribe를 잡아서 등록해주어야 한다.
		char	cmdstr[80];
		sprintf (cmdstr, "%.*s", args[0]->len, args[0]->ptr);
		if (strcasecmp (cmdstr, "subscribe") == 0)	{
			if (reply->type == REPLY_ARRAY)	{
				for (size_t i=1;i<reply->elements;i+=3)	{
					register_subscribe (client_fd, mutex, reply->element[i]->ptr, reply->element[i]->len);
				}
			}
		}
		else if (strcasecmp (cmdstr, "unsubscribe") == 0)	{
			if (reply->type == REPLY_INTEGER && reply->integer > 0)	{
				for (uint32_t i = 1;i<argc;i++)	{
					unregister_subscribe (client_fd, args[i]);
				}
			}
		}
	}

	pthread_mutex_lock (mutex);
    resp_send(client_fd, reply);
	pthread_mutex_unlock (mutex);
    reply_free(reply);

    // 정리
    for (uint32_t i = 0; i < argc; i++) {
        if (args[i]) {
            free((void*)args[i]->ptr);
            free(args[i]);
        }
    }
}

#define	__max__(a,b) ((a)>(b)?(a):(b))
/* ============================================================
 *  클라이언트 핸들러
 * ============================================================ */
static void *client_handler(void *arg)
{
    int client_fd = (int)(intptr_t)arg;
    RespParser *parser = resp_parser_new();
    char temp[BUFFER_SIZE];
    ssize_t n;

	pthread_mutex_t	mutex;
	pthread_mutex_init (&mutex, NULL);
    printf("[INFO] Client connected (fd=%d|pid:%ld) \n", client_fd, syscall(SYS_gettid));

	fd_set	fdset;
	int		maxfd = -1;
    while (running) {
		FD_ZERO (&fdset);
		maxfd = __max__(client_fd, maxfd);
		FD_SET (client_fd, &fdset);
		int	rc = select (maxfd + 1, &fdset, NULL, NULL, NULL);
		if (rc <= 0)	continue;
		n = read(client_fd, temp, sizeof(temp) - 1);
		if (n <= 0) break;

		resp_parser_append(parser, temp, n);

		while (1) {
			size_t old_pos = parser->pos;
			process_command(&mutex, client_fd, g_shm, parser);

			if (parser->pos == old_pos) break;  // 더 이상 파싱할 수 없음
		}

		// 처리된 데이터 제거
		if (parser->pos > 0) {
			memmove(parser->buffer, parser->buffer + parser->pos, parser->len - parser->pos);
			parser->len -= parser->pos;
			parser->pos = 0;
		}
    }

    resp_parser_free(parser);
	fd_clear_subscribe (client_fd);
    close(client_fd);
    printf("[INFO] Client disconnected (fd=%d)\n", client_fd);
    return NULL;
}

int	verbose = DBG_INFO;
int	port = DEFAULT_PORT;
int daemonflag;
char	*mredis_name;

int	getoption (int argc, char *argv[])	{
	int	n = 1;
	while (n < argc)	{
		if (strcasecmp (argv[n], "--verbose") == 0)	{
			n ++; if (n >= argc)	return -1;
			if (isdigit(argv[n][0]))	{
				verbose = atoi(argv[n]);
			}
			else	{
				if (strcasecmp (argv[n], "DBG_NONE") == 0)	verbose = DBG_NONE;
				else if (strcasecmp (argv[n], "DBG_ERROR") == 0)	verbose = DBG_ERROR;
				else if (strcasecmp (argv[n], "DBG_WARN") == 0)		verbose = DBG_WARN;
				else if (strcasecmp (argv[n], "DBG_INFO") == 0)		verbose = DBG_INFO;
				else if (strcasecmp (argv[n], "DBG_TRACE") == 0)	verbose = DBG_TRACE;
			}
		}
		else if (strcasecmp (argv[n], "--name") == 0)	{
			n ++; if (n >= argc)	return -1;
			mredis_name = argv[n];
		}
		else if (strcasecmp (argv[n], "--port") == 0)	{
			n ++; if (n >= argc)	return -1;
			port = atoi (argv[n]);
		}
		else if (strcasecmp (argv[n], "--daemon") == 0)	{
			daemonflag = 1;
		}
		n ++;
	}
	return 0;
}

void	usage(char *pname)	{
	printf ("%s --port [portno] --verbose --daemon\n", basename (pname));
	printf ("\tverbose [DBG_NONE|DBG_ERROR|DBG_WARN|DBG_INFO|DBG_TRACE]\n");
	return;
}
/* ============================================================
 *  MAIN
 * ============================================================ */
int main(int argc, char *argv[])
{
	mredis_name = SHM_DEFAULT_NAME;
	int rc = getoption (argc, argv);
	if (rc < 0)	{
		usage(argv[0]);
		return 0;
	}

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

	mredis_set_debug_level(verbose);
    printf("==================================================================\n");
    printf("     MREDIS RESP Server (Redis Compatible) pid:%d\n", getpid());
    printf("==================================================================\n");

    // SHM 열기 또는 생성
    g_shm = mredis_open_existing(mredis_name);
    if (!g_shm) {
        printf("[INFO] SHM이 없어 새로 생성합니다... (1GB)\n");
        g_shm = mredis_create(mredis_name, 1ULL << 30);  // 1GB
    }

    if (!g_shm) {
        fprintf(stderr, "SHM 초기화 실패!\n");
        return 1;
    }

    mredis_dump_stats(g_shm);
	sigfd = make_signalfd();
	if (sigfd < 0)	return -1;

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
	pthread_t stid;
	pthread_create (&stid, NULL, subscribe_manager, NULL);
	pthread_t ctid;
	pthread_create (&ctid, NULL, subscribe_cleaner, NULL);

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
    mredis_close(g_shm);
    printf("서버 종료됨.\n");
    return 0;
}
