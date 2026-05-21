#ifndef RDB_H
#define RDB_H
/*
 * rdb.h  –  RDB (Redis Database) Snapshot for mredis
 *
 * ┌──────────────────────────────────────────────────────────────────┐
 * │  파일 포맷 (바이너리, little-endian)                              │
 * │                                                                  │
 * │  [RdbFileHeader]                                                 │
 * │  반복: [RdbRecordHeader + payload]                               │
 * │  [RDB_TYPE_EOF][uint32_t crc32]                                  │
 * │                                                                  │
 * │  RdbRecordHeader                                                 │
 * │    uint8_t  type     RDB_TYPE_*                                  │
 * │    uint32_t klen     키 길이                                     │
 * │    char     key[]    키 데이터 (null 종단 없음)                  │
 * │                                                                  │
 * │  payload (type 별)                                               │
 * │    KV   : uint32_t vlen + char val[]                             │
 * │    ZSET : uint64_t count + count×(double score + str member)     │
 * │    HASH : uint64_t count + count×(str field + str val)           │
 * │                                                                  │
 * │  str = uint32_t len + char data[]  (null 종단 없음)              │
 * └──────────────────────────────────────────────────────────────────┘
 */

#include <stdint.h>
#include <stddef.h>
#include "shm_types.h"
#include "shm_core.h"

/* ── 매직 / 버전 ────────────────────────────────────────── */
#define RDB_MAGIC       "MREDIS"
#define RDB_MAGIC_LEN   6
#define RDB_VERSION     1

#define	ENTRY_EOF		0xff
/* ── 기본 경로 ───────────────────────────────────────────── */
#define RDB_DEFAULT_PATH "mredis.rdb"
#define RDB_TMP_SUFFIX   ".tmp"

/* ── 파일 헤더 (on-disk) ─────────────────────────────────── */
typedef struct __attribute__((packed)) {
    char     magic[RDB_MAGIC_LEN];   /* "MREDIS"              */
    uint16_t version;                 /* RDB_VERSION           */
    uint64_t created_at;              /* unix timestamp (sec)  */
    uint64_t key_count;               /* 총 키 수              */
    uint8_t  reserved[14];            /* 향후 확장용           */
} RdbFileHeader;                      /* = 36 bytes            */

/* ── RDB 핸들 ────────────────────────────────────────────── */
typedef struct {
    char    *path;          /* RDB 파일 경로                   */
    int      enabled;       /* 0=비활성, 1=활성                */

    /* BGSAVE 상태 */
    pid_t    bg_child;      /* fork 된 자식 PID (-1 = 없음)   */
    int      saving;        /* 1 = BGSAVE 진행 중              */
    time_t   last_save;     /* 마지막 성공 저장 시각           */
    int      last_status;   /* 마지막 저장 결과 (0=성공)       */

    /* 통계 */
    uint64_t save_count;    /* 성공한 저장 횟수                */
    uint64_t key_count;     /* 마지막 저장 당시 키 수          */
    uint64_t file_size;     /* 마지막 저장 파일 크기 (bytes)   */
} RdbHandle;

/* ════════════════════════════════════════════════════════════
 *  공개 API
 * ════════════════════════════════════════════════════════════ */

/*
 * rdb_open  –  RDB 핸들 생성.
 *   path: 파일 경로 (NULL → RDB_DEFAULT_PATH)
 */
RdbHandle *rdb_open(const char *path);

/*
 * rdb_close  –  핸들 해제.
 */
void rdb_close(RdbHandle *rdb);

/*
 * rdb_save  –  SHM → RDB 파일 저장 (포그라운드, 동기).
 *   tmp 파일에 쓰고 완료 후 atomic rename.
 *   반환: SHM_OK / SHM_ERR
 */
int rdb_save(RdbHandle *rdb, ShmHandle *shm);

/*
 * rdb_load  –  RDB 파일 → SHM 적재 (서버 시작 시).
 *   반환: 적재된 키 수 (실패 시 음수)
 */
int64_t rdb_load(RdbHandle *rdb, ShmHandle *shm);

/*
 * rdb_save_bg  –  BGSAVE: fork() → 자식이 rdb_save 수행.
 *   반환: SHM_OK (fork 성공) / SHM_ERR
 */
int rdb_save_bg(RdbHandle *rdb, ShmHandle *shm);

/*
 * rdb_save_check  –  BGSAVE 자식 완료 여부 확인 (non-blocking).
 *   메인 루프에서 주기적으로 호출.
 *   반환: 1=완료, 0=진행중, -1=에러
 */
int rdb_save_check(RdbHandle *rdb);

/*
 * rdb_stats  –  통계 문자열 출력.
 */
void rdb_stats(RdbHandle *rdb, char *buf, size_t blen);

/*
 * rdb_crc32  –  CRC-32 계산 (파일 무결성 검증용).
 */
uint32_t rdb_crc32(const void *data, size_t len, uint32_t crc);

#endif /* RDB_H */
