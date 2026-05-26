# ═══════════════════════════════════════════════════════
#  mredis Makefile
# ═══════════════════════════════════════════════════════

CC      = gcc
#CFLAGS  = -Wall -Wextra -fPIC -O2 -g -std=c11 -D_GNU_SOURCE -D_REENTRANT
CFLAGS  = -Wall -fPIC -O2 -g -std=c11 -D_GNU_SOURCE -D_REENTRANT
LDFLAGS = -lrt -lpthread -lm

# ── 컴파일 파라미터 ──────────────────────────────────────
#
#  CI 빌드  (작은 SHM, 테스트 전용)
#    HASH_TABLE_SIZE=0x10000   = 64K 버킷  (SHM 약 3MB)
#    HASH_FIELD_BUCKETS=16
#
#  PROD 빌드 (운영, 16M 버킷 기본값)
#    HASH_TABLE_SIZE=0x1000000 = 16M 버킷  (SHM 약 1GB+)
#    HASH_FIELD_BUCKETS=16
#
CI_DEFS  = -DHASH_TABLE_SIZE=0x10000ULL \
           -DHASH_FIELD_BUCKETS=16U      \
           -DSHM_DEBUG_LEVEL=1

PROD_DEFS = -DHASH_FIELD_BUCKETS=16U \
            -DSHM_DEBUG_LEVEL=1

# ── 소스 파일 ────────────────────────────────────────────
CORE_SRCS = shm_core.c siphash.c
CMD_SRCS  = cmd_kv.c cmd_zset.c cmd_hash.c cmd_keys.c \
            cmd_del.c cmd_dispatch.c cmd_pubsub.c cmd_bset.c

ALL_SRCS  = $(CORE_SRCS) $(CMD_SRCS)
ALL_OBJS  = $(ALL_SRCS:.c=.o)

# ── 헤더 (변경 시 전체 재빌드) ───────────────────────────
HDRS = shm_types.h shm_core.h \
       cmd_kv.h cmd_zset.h cmd_hash.h cmd_keys.h \
       cmd_del.h cmd_dispatch.h cmd_pubsub.h cmd_bset.h

.PHONY: all ci prod test clean

# 기본 타겟: CI 빌드로 테스트
all: ci lib

lib: $(ALL_OBJS)
	ar -ruv libmredis.a $(ALL_OBJS)
	$(CC) -shared -o libmredis.so $(ALL_OBJS) $(LDFLAGS) -rdynamic

# ── CI: 테스트 빌드 + 실행 ──────────────────────────────
ci: test_all_ci
	@echo ""
	@echo "────────── 테스트 실행 ──────────"

test_all_ci: $(ALL_SRCS) test_all.c $(HDRS)
	$(CC) $(CFLAGS) $(CI_DEFS) -o $@ \
	    $(ALL_SRCS) test_all.c $(LDFLAGS)

# ── PROD: 운영 서버 빌드 ────────────────────────────────
prod: resp_server
resp_server: $(ALL_SRCS) resp_server.c $(HDRS)
	$(CC) $(CFLAGS) $(PROD_DEFS) -o $@ \
	    $(ALL_SRCS) resp_server.c $(LDFLAGS)

pub: test_pubsub
test_pubsub: $(ALL_SRCS) test_pubsub.c $(HDRS)
	$(CC) $(CFLAGS) $(CI_DEFS) -o $@ \
	    $(ALL_SRCS) test_pubsub.c $(LDFLAGS)

# ── test 타겟 (= ci) ────────────────────────────────────
test: ci

# ── 개별 오브젝트 빌드 규칙 (필요 시) ──────────────────
%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(CI_DEFS) -c -o $@ $<

# ── 정리 ────────────────────────────────────────────────
clean:
	rm -f test_all_ci resp_server *.o stress test_pubsub
	-rm -f /dev/shm/shm_mredis_test 2>/dev/null || true
	@echo "정리 완료"

stress:	$(ALL_SRCS) stress.c $(HDRS)
	$(CC) $(CFLAGS) $(CI_DEFS) -o $@ \
	    $(ALL_SRCS) stress.c $(LDFLAGS)

# ── 도움말 ──────────────────────────────────────────────
help:
	@echo "사용법:"
	@echo "  make          – CI 빌드 + 테스트 실행"
	@echo "  make ci       – CI 빌드 + 테스트 실행"
	@echo "  make prod     – 운영 서버(resp_server) 빌드"
	@echo "  make test     – 테스트만 실행"
	@echo "  make clean    – 빌드 결과물 삭제"
