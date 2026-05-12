/*
 * cmd_dispatch.c  –  커맨드 라우팅 테이블 구현
 */
#define _GNU_SOURCE
#include <string.h>
#include <strings.h>
#include "shm_types.h"
#include "shm_core.h"
#include "cmd_keys.h"
#include "cmd_kv.h"
#include "cmd_zset.h"
#include "cmd_hash.h"
#include "cmd_set.h"
#include "cmd_dispatch.h"

/* ============================================================
 *  커맨드 테이블
 *  min_argc 는 args[0](커맨드 이름) 포함 최소 인자 수.
 *  각 cmd_*() 함수가 내부적으로도 인자 수를 검사하므로
 *  여기서는 초기 빠른 거부를 위한 최솟값만 지정.
 * ============================================================ */
static const CmdEntry g_cmd_table[] = {
    /* ── KV ──────────────────────────────────────────────── */
    { "KEYS",           cmd_keys,           2, "KEYS pattern"                         },
    { "SET",            cmd_set,            3, "SET key value"                        },
    { "GET",            cmd_get,            2, "GET key"                              },
    { "MSET",           cmd_mset,           3, "MSET key1 value1 [key2 value2 ...]"   },
    { "MGET",           cmd_mget,           2, "MGET key1 [key2 ...]"                 },
    { "KDEL",           cmd_kdel,           2, "KDEL key [key …]"                     },

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
	{ "SCREATE",        cmd_screate,        2, "SCREATE key"                          },
    { "SDROP",          cmd_sdrop,          2, "SDROP key"                            },
    { "SADD",           cmd_sadd,           3, "SADD key member [...]"                },
    { "SREM",           cmd_srem,           3, "SREM key member [...]"                },
    { "SISMEMBER",      cmd_sismember,      3, "SISMEMBER key member"                 },
    { "SCARD",          cmd_scard,          2, "SCARD key"                            },
    { "SMEMBERS",       cmd_smembers,       2, "SMEMBERS key"                         },
    { "SPOP",           cmd_spop,           2, "SPOP key [count]"                     },
    { "SRANDMEMBER",    cmd_srandmember,    2, "SRANDMEMBER key [count]"              },
};

static const size_t g_cmd_count =
    sizeof(g_cmd_table) / sizeof(g_cmd_table[0]);

static const EraseEntry g_erase_table[] = {
    { ENTRY_KV,           cmd_kdel            },
    { ENTRY_ZSET,         cmd_zdrop           },
    { ENTRY_HASH,         cmd_hdrop           },
    { ENTRY_SET,          cmd_sdrop           },
};
static const size_t g_erase_count =
    sizeof(g_erase_table) / sizeof(g_erase_table[0]);

/* ── DEL ─────────────────────────────────────────────────────
 *  args: DEL key [key …]
 *  반환: INTEGER 삭제 수
 * ─────────────────────────────────────────────────────────── */
s_replyObject *cmd_del(ShmHandle *h, string_t *args[], uint32_t argc)
{
    if (argc < 2) return reply_error(SHM_ERR_ARGC, "usage: DEL key [key …]");

    int64_t removed = 0;
    for (uint32_t a = 1; a < argc; a++) {
        const void *key = args[a]->ptr; uint32_t klen = args[a]->len;
        if (klen == 0) continue;

        uint32_t idx    = shm_hash(key, klen);
        BucketEntry *bk = core_get_bucket(h, idx);
        bucket_lock(h, idx);

		uint32_t entry = bucket_find_entry_number(h, bk, key, klen);
        pthread_mutex_unlock(&bk->mutex);
        if (entry == UINT32_MAX) continue;

		for (uint32_t j = 0; j < g_erase_count; j++)	{
			if (entry == g_erase_table[j].entry_number)	{
				string_t *get_args[2] = {
					&STR_LIT("DEL"),
					args[a]
				};
				s_replyObject *r = g_erase_table[j].fn(h, get_args, 2);
				if (r && r->type == REPLY_INTEGER && r->integer == 1) {
					LOG_TRACE("DEL: '%.*s'", klen, (const char *)key);
					removed++;
					break;
				}
			}
		}
    }
    return reply_integer(removed);
}
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
	if (strcmp (name, "DEL") == 0) return cmd_del(h, args, argc);
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
