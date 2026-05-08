/*
 * cmd_dispatch.c  –  커맨드 라우팅 테이블 구현
 */
#define _GNU_SOURCE
#include <string.h>
#include <strings.h>
#include "shm_types.h"
#include "shm_core.h"
#include "cmd_kv.h"
#include "cmd_zset.h"
#include "cmd_hash.h"
#include "cmd_dispatch.h"

/* ============================================================
 *  커맨드 테이블
 *  min_argc 는 args[0](커맨드 이름) 포함 최소 인자 수.
 *  각 cmd_*() 함수가 내부적으로도 인자 수를 검사하므로
 *  여기서는 초기 빠른 거부를 위한 최솟값만 지정.
 * ============================================================ */
static const CmdEntry g_cmd_table[] = {
    /* ── KV ──────────────────────────────────────────────── */
    { "SET",            cmd_set,            3, "SET key value"                        },
    { "GET",            cmd_get,            2, "GET key"                              },
    { "DEL",            cmd_del,            2, "DEL key [key …]"                      },

    /* ── Sorted Set ─────────────────────────────────────── */
    { "ZCREATE",        cmd_zcreate,        2, "ZCREATE key"                          },
    { "ZDROP",          cmd_zdrop,          2, "ZDROP key"                            },
    { "ZADD",           cmd_zadd,           4, "ZADD key [NX|XX|GT|LT|CH] score member [score member …]" },
    { "ZREM",           cmd_zrem,           3, "ZREM key member [member …]"           },
    { "ZSCORE",         cmd_zscore,         3, "ZSCORE key member"                    },
    { "ZINCRBY",        cmd_zincrby,        4, "ZINCRBY key delta member"             },
    { "ZRANK",          cmd_zrank,          3, "ZRANK key member"                     },
    { "ZREVRANK",       cmd_zrevrank,       3, "ZREVRANK key member"                  },
    { "ZCARD",          cmd_zcard,          2, "ZCARD key"                            },
    { "ZCOUNT",         cmd_zcount,         4, "ZCOUNT key min max"                   },
    { "ZRANGE",         cmd_zrange,         4, "ZRANGE key start stop [REV]"          },
    { "ZRANGEBYSCORE",  cmd_zrangebyscore,  4, "ZRANGEBYSCORE key min max [REV] [LIMIT offset count]" },
    { "ZPOPMIN",        cmd_zpopmin,        2, "ZPOPMIN key [count]"                  },
    { "ZPOPMAX",        cmd_zpopmax,        2, "ZPOPMAX key [count]"                  },

    /* ── Hash Map ───────────────────────────────────────── */
    { "HCREATE",        cmd_hcreate,        2, "HCREATE key"                          },
    { "HDROP",          cmd_hdrop,          2, "HDROP key"                            },
    { "HSET",           cmd_hset,           4, "HSET key field value [field value …]" },
    { "HGET",           cmd_hget,           3, "HGET key field"                       },
    { "HDEL",           cmd_hdel,           3, "HDEL key field [field …]"             },
    { "HEXISTS",        cmd_hexists,        3, "HEXISTS key field"                    },
    { "HLEN",           cmd_hlen,           2, "HLEN key"                             },
    { "HGETALL",        cmd_hgetall,        2, "HGETALL key"                          },
    { "HKEYS",          cmd_hkeys,          2, "HKEYS key"                            },
    { "HVALS",          cmd_hvals,          2, "HVALS key"                            },
    { "HINCRBY",        cmd_hincrby,        4, "HINCRBY key field delta"              },
    { "HINCRBYFLOAT",   cmd_hincrbyfloat,   4, "HINCRBYFLOAT key field delta"         },
};

static const size_t g_cmd_count =
    sizeof(g_cmd_table) / sizeof(g_cmd_table[0]);

/* ============================================================
 *  공개 API
 * ============================================================ */
const CmdEntry *cmd_table_get(size_t *out_count)
{
    if (out_count) *out_count = g_cmd_count;
    return g_cmd_table;
}

s_replyObject *cmd_dispatch(ShmHandle *h,
                              string_t  *args[],
                              uint32_t   argc)
{
    if (!h || !args || argc == 0 || !args[0])
        return reply_error(SHM_ERR_INVAL, "빈 커맨드");

    const char *name = args[0]->ptr;
    for (size_t i = 0; i < g_cmd_count; i++) {
        if (strcasecmp(name, g_cmd_table[i].name) == 0) {
            /* 최소 인자 수 검사 */
            if (argc < g_cmd_table[i].min_argc) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "인자 부족 – usage: %s", g_cmd_table[i].usage);
                return reply_error(SHM_ERR_ARGC, msg);
            }
            /* 커맨드 실행 */
            LOG_TRACE("DISPATCH: %s (argc=%u)", name, argc);
            return g_cmd_table[i].fn(h, args, argc);
        }
    }
    char msg[128];
    snprintf(msg, sizeof(msg), "알 수 없는 커맨드 '%s'", name);
    return reply_error(SHM_ERR_INVAL, msg);
}
