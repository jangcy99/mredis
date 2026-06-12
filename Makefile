# ═══════════════════════════════════════════════════════
#  mredis Makefile
# ═══════════════════════════════════════════════════════
CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -fPIC -g -std=c11 -D_GNU_SOURCE -I$(JAVA_HOME)/include -I$(JAVA_HOME)/include/linux
LDFLAGS = -lrt -lpthread -lm

CI_DEFS  = -DHASH_TABLE_SIZE=0x10000ULL \
           -DHASH_FIELD_BUCKETS=16U      \
           -DSHM_DEBUG_LEVEL=3

PROD_DEFS = -DHASH_FIELD_BUCKETS=16U \
            -DSHM_DEBUG_LEVEL=1

HDRS = mredis_types.h mredis_core.h \
       cmd_kv.h cmd_zset.h cmd_hash.h cmd_keys.h \
       cmd_del.h cmd_bset.h cmd_cset.h cmd_dispatch.h skiplist.h cmd_set.h

CORE_SRCS = mredis_core.c siphash.c skiplist.c
CMD_SRCS  = cmd_kv.c cmd_zset.c cmd_hash.c cmd_keys.c \
            cmd_del.c cmd_bset.c cmd_cset.c cmd_dispatch.c cmd_pubsub.c MRedisNative.c \
			cmd_set.c
ALL_SRCS  = $(CORE_SRCS) $(CMD_SRCS)
ALL_OBJS  = $(ALL_SRCS:.c=.o)

.PHONY: all ci test clean bset cset pubsub help

all: static shared ci

%.o:%.c
	$(CC) $(CFLAGS) $(CI_DEFS) -c -o $@ $<

static: $(ALL_OBJS)
	ar ruv libmredis.a $(ALL_OBJS)

shared:	libmredis.so.1

libmredis.so.1:	$(ALL_OBJS)
	$(CC) -shared -Wl,-soname,libmredis.so.1 -o $@ $^ $(LDFLAGS)
	ln -sf libmredis.so.1 libmredis.so
	ln -sf libmredis.so.1 libmredis.so.1.1


# ── 전체 통합 테스트 ─────────────────────────────────────
ci: test_all_ci
	@echo ""
	@echo "────────── 테스트 실행 ──────────"
	./test_all_ci

test_all_ci: $(ALL_SRCS) test_all.c $(HDRS)
	$(CC) $(CFLAGS) $(CI_DEFS) -o $@ \
	    $(ALL_SRCS) test_all.c $(LDFLAGS)

test: ci

# ── BSET 전용 테스트 ─────────────────────────────────────
test_bset: $(ALL_SRCS) test_bset.c $(HDRS)
	$(CC) $(CFLAGS) $(CI_DEFS) -o $@ \
	    $(ALL_SRCS) test_bset.c $(LDFLAGS)

bset: test_bset
	./test_bset

# ── Pub/Sub 테스트 ───────────────────────────────────────
test_pubsub: $(ALL_SRCS) cmd_pubsub.c test_pubsub.c $(HDRS) cmd_pubsub.h
	$(CC) $(CFLAGS) $(CI_DEFS) -o $@ \
	    $(ALL_SRCS) cmd_pubsub.c test_pubsub.c $(LDFLAGS)

pubsub: test_pubsub
	./test_pubsub

# ── 운영 서버 ────────────────────────────────────────────
prod: resp_server
resp_server: $(ALL_SRCS) resp_server.c $(HDRS)
	$(CC) $(CFLAGS) $(PROD_DEFS) -o $@ \
	    $(ALL_SRCS) resp_server.c $(LDFLAGS)

# ── 정리 ────────────────────────────────────────────────
clean:
	rm -f test_all_ci test_bset test_pubsub resp_server *.o
	-rm -f /dev/shm/mredis_mredis_all_test \
	        /dev/shm/mredis_bset_test \
	        /dev/shm/mredis_pubsub_test 2>/dev/null || true
	-rm 0f *.so *.a
	@echo "정리 완료"

help:
	@echo "사용법:"
	@echo "  make          – 전체 통합 테스트 (§01~§10)"
	@echo "  make bset     – BSET 전용 테스트"
	@echo "  make pubsub   – Pub/Sub 테스트"
	@echo "  make prod     – 운영 서버(resp_server) 빌드"
	@echo "  make clean    – 빌드 결과물 삭제"
