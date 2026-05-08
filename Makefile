CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -g
LDFLAGS = -lrt -lpthread -lm

# ── 소스 파일 ─────────────────────────────────────────────
CORE_SRCS  = shm_core.c
CMD_SRCS   = cmd_kv.c cmd_zset.c cmd_hash.c cmd_dispatch.c
TEST_SRCS  = test_all.c
ALL_SRCS   = $(CORE_SRCS) $(CMD_SRCS) $(TEST_SRCS)

# ── 운영 빌드 (4GB, 16M 버킷) ─────────────────────────────
TARGET     = test_all
PROD_FLAGS = -DSHM_DEBUG_LEVEL=3

# ── CI 빌드 (64MB, 64K 버킷) ──────────────────────────────
TARGET_CI  = test_ci
CI_FLAGS   = -DHASH_TABLE_SIZE=0x100000ULL \
             -DHASH_FIELD_BUCKETS=16U \
             -DSHM_DEBUG_LEVEL=3

.PHONY: all ci clean run run_ci

# 기본: CI 빌드 (메모리 절약)
all: $(TARGET_CI)

$(TARGET_CI): $(ALL_SRCS) shm_types.h shm_core.h cmd_kv.h cmd_zset.h cmd_hash.h cmd_dispatch.h
	$(CC) $(CFLAGS) $(CI_FLAGS) -o $@ \
	    $(CORE_SRCS) $(CMD_SRCS) $(TEST_SRCS) $(LDFLAGS)

run_ci: $(TARGET_CI)
	./$(TARGET_CI)

# 운영 빌드
prod: $(TARGET)

$(TARGET): $(ALL_SRCS) shm_types.h shm_core.h cmd_kv.h cmd_zset.h cmd_hash.h cmd_dispatch.h
	$(CC) $(CFLAGS) $(PROD_FLAGS) -o $@ \
	    $(CORE_SRCS) $(CMD_SRCS) $(TEST_SRCS) $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

server: $(ALL_SRCS) resp_server.c
	$(CC) $(CFLAGS) $(PROD_FLAGS) -o shm_server \
	    shm_core.c cmd_kv.c cmd_zset.c cmd_hash.c cmd_dispatch.c resp_server.c \
	    -lrt -lpthread -lm

run_server:
	./shm_server

clean:
	rm -f $(TARGET) $(TARGET_CI) *.o
	-rm -f /dev/shm/shm_v5_test 2>/dev/null || true
