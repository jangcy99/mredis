/*
 * cmd_dispatch.c  –  커맨드 라우팅 테이블
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "mredis_types.h"
#include "mredis_core.h"
#include "cmd_kv.h"
#include "cmd_zset.h"
#include "cmd_hash.h"
#include "cmd_keys.h"
#include "cmd_del.h"
#include "cmd_set.h"
#include "cmd_bset.h"
#include "cmd_cset.h"
#include "cmd_dispatch.h"

static const CmdEntry g_cmd_table[] = {
    /* ── KV ──────────────────────────────────────────────── */
    { "SET",             cmd_set,             3, "SET key value"                          },
    { "GET",             cmd_get,             2, "GET key"                                },
    { "DEL",             cmd_del,             2, "DEL key [key …]"                        },
    { "KEYS",            cmd_keys,            2, "KEYS pattern"                           },

    /* ── Sorted Set ─────────────────────────────────────── */
    { "ZCREATE",         cmd_zcreate,         2, "ZCREATE key"                            },
    { "ZDROP",           cmd_zdrop,           2, "ZDROP key"                              },
    { "ZADD",            cmd_zadd,            4, "ZADD key score member [score member …]" },
    { "ZREM",            cmd_zrem,            3, "ZREM key member [member …]"             },
    { "ZSCORE",          cmd_zscore,          3, "ZSCORE key member"                      },
    { "ZINCRBY",         cmd_zincrby,         4, "ZINCRBY key delta member"               },
    { "ZRANK",           cmd_zrank,           3, "ZRANK key member"                       },
    { "ZREVRANK",        cmd_zrevrank,        3, "ZREVRANK key member"                    },
    { "ZCARD",           cmd_zcard,           2, "ZCARD key"                              },
    { "ZCOUNT",          cmd_zcount,          4, "ZCOUNT key min max"                     },
    { "ZRANGE",          cmd_zrange,          4, "ZRANGE key start stop [REV]"            },
    { "ZRANGEBYSCORE",   cmd_zrangebyscore,   4, "ZRANGEBYSCORE key min max [LIMIT …]"    },
    { "ZPOPMIN",         cmd_zpopmin,         2, "ZPOPMIN key [count]"                    },
    { "ZPOPMAX",         cmd_zpopmax,         2, "ZPOPMAX key [count]"                    },

    /* ── Hash Map ───────────────────────────────────────── */
    { "HCREATE",         cmd_hcreate,         2, "HCREATE key"                            },
    { "HDROP",           cmd_hdrop,           2, "HDROP key"                              },
    { "HSET",            cmd_hset,            4, "HSET key field value [field value …]"   },
    { "HGET",            cmd_hget,            3, "HGET key field"                         },
    { "HDEL",            cmd_hdel,            3, "HDEL key field [field …]"               },
    { "HEXISTS",         cmd_hexists,         3, "HEXISTS key field"                      },
    { "HLEN",            cmd_hlen,            2, "HLEN key"                               },
    { "HGETALL",         cmd_hgetall,         2, "HGETALL key"                            },
    { "HKEYS",           cmd_hkeys,           2, "HKEYS key"                              },
    { "HVALS",           cmd_hvals,           2, "HVALS key"                              },
    { "HINCRBY",         cmd_hincrby,         4, "HINCRBY key field delta"                },
    { "HINCRBYFLOAT",    cmd_hincrbyfloat,    4, "HINCRBYFLOAT key field delta"           },

    /* ── Binary Sorted Set (BSET) – 배열 기반, rwlock ─── */
    { "BSET",            cmd_bset,            4, "BSET key score value [score value …]"   },
    { "BGET",            cmd_bget,            3, "BGET key score"                         },
    { "BDEL",            cmd_bdel,            3, "BDEL key score [score …]"               },
    { "BRANGE",          cmd_brange,          4, "BRANGE key start stop"                  },
    { "BRANGEBYSCORE",   cmd_brangebyscore,   4, "BRANGEBYSCORE key min max [LIMIT …]"    },
    { "BCARD",           cmd_bcard,           2, "BCARD key"                              },
    { "BRANK",           cmd_brank,           3, "BRANK key score"                        },
    { "BCOUNT",          cmd_bcount,          4, "BCOUNT key min max"                     },
    { "BPOPMIN",         cmd_bpopmin,         2, "BPOPMIN key [count]"                    },
    { "BPOPMAX",         cmd_bpopmax,         2, "BPOPMAX key [count]"                    },
    { "BDROP",           cmd_bdrop,           2, "BDROP key"                              },

    /* ── Chunk Sorted Set (CSET) – 청크 체인, 삭제비트 ── */
    { "CSET",            cmd_cset,            4, "CSET key score value [score value …]"   },
    { "CGET",            cmd_cget,            3, "CGET key score"                         },
    { "CDEL",            cmd_cdel,            3, "CDEL key score [score …]"               },
    { "CRANGE",          cmd_crange,          4, "CRANGE key start stop"                  },
    { "CRANGEBYSCORE",   cmd_crangebyscore,   4, "CRANGEBYSCORE key min max [LIMIT …]"    },
    { "CCARD",           cmd_ccard,           2, "CCARD key"                              },
    { "CRANK",           cmd_crank,           3, "CRANK key score"                        },
    { "CCOUNT",          cmd_ccount,          4, "CCOUNT key min max"                     },
    { "CPOPMIN",         cmd_cpopmin,         2, "CPOPMIN key [count]"                    },
    { "CPOPMAX",         cmd_cpopmax,         2, "CPOPMAX key [count]"                    },
    { "CDROP",           cmd_cdrop,           2, "CDROP key"                              },
    { "CCOMPACT",        cmd_ccompact,        2, "CCOMPACT key"                           },

	/* -- SET - 집합구현 ---*/
    { "SADD",            cmd_sadd,            3, "SADD key [memver ...]"                  },
    { "SREM",            cmd_srem,            3, "SREM key [member ...]"                  },
    { "SISMEMBER",       cmd_sismember,       3, "SISMEMBER key member"                   },
    { "SCARD",           cmd_scard,           2, "SCARD key"                              },
    { "SMEMBERS",        cmd_smembers,        2, "SMEMBERS key"                           },
    { "SPOP",            cmd_spop,            2, "SPOP key [count]"                       },
    { "SRANDMEMBER",     cmd_srandmember,     2, "SRANDMEMBER key"                        },
    { "SUNION",          cmd_sunion,          3, "SUNION key [key ...]"                   },
    { "SINTER",          cmd_sinter,          3, "SINTER key [key ...]"                   },
    { "SDIFF",           cmd_sdiff,           3, "SDIFF key [key ...]"                    },
};

static const size_t g_cmd_count = sizeof(g_cmd_table)/sizeof(g_cmd_table[0]);

const CmdEntry *cmd_table_get(size_t *out_count) {
    if (out_count) *out_count = g_cmd_count;
    return g_cmd_table;
}

s_replyObject *cmd_dispatch(MRedisHandle *h, string_t *args[], uint32_t argc) {
    if (!h || !args || argc==0 || !args[0])
        return reply_error(SHM_ERR_INVAL, "빈 커맨드");
    const char *name = args[0]->ptr;
    for (size_t i = 0; i < g_cmd_count; i++) {
        if (strcasecmp(name, g_cmd_table[i].name)==0) {
            if (argc < g_cmd_table[i].min_argc) {
                char msg[128];
                snprintf(msg,sizeof(msg),"인자 부족 – usage: %s",g_cmd_table[i].usage);
                return reply_error(SHM_ERR_ARGC, msg);
            }
            LOG_TRACE("DISPATCH: %s (argc=%u)", name, argc);
            return g_cmd_table[i].fn(h, args, argc);
        }
    }
    char msg[128];
    snprintf(msg,sizeof(msg),"알 수 없는 커맨드 '%s'",name);
    return reply_error(SHM_ERR_INVAL, msg);
}
