# mredis
현재 구현되어 있는 항목
    ── KV ────────────────────────────────────────────────
    KEYS           : KEYS pattern
    SET            : SET key value
    GET            : GET key
    MSET           : MSET key1 value1 [key2 value2 ...]
    MGET           : MGET key1 [key2 ...]
    DEL            : DEL key [key …]
    DEL            : DEL key [key …]

    ── Sorted Set ─: ──────────────
    ZCREATE        : ZCREATE key
    ZDROP          : ZDROP key
    ZADD           : ZADD key [NX|XX|GT|LT|CH] score member [score member …]
    ZREM           : ZREM key member [member …]
    ZSCORE         : ZSCORE key member
    ZINCRBY        : ZINCRBY key delta member
    ZRANK          : ZRANK key member
    ZREVRANK       : ZREVRANK key member
    ZCARD          : ZCARD key
    ZCOUNT         : ZCOUNT key min max
    ZRANGE         : ZRANGE key start stop [REV]
    ZRANGEBYSCORE  : ZRANGEBYSCORE key min max [REV] [LIMIT offset count]
    ZPOPMIN        : ZPOPMIN key [count]
    ZPOPMAX        : ZPOPMAX key [count]

    ── Hash Map ───: ──────────────
    HCREATE        : HCREATE key
    HDROP          : HDROP key
    HSET           : HSET key field value [field value …]
    HGET           : HGET key field
    HDEL           : HDEL key field [field …]
    HEXISTS        : HEXISTS key field
    HLEN           : HLEN key
    HGETALL        : HGETALL key
    HKEYS          : HKEYS key
    HVALS          : HVALS key
    HINCRBY        : HINCRBY key field delta
    HINCRBYFLOAT   : HINCRBYFLOAT key field delta
	SCREATE        : SCREATE key
    SDROP          : SDROP key
    SADD           : SADD key member [...]
    SREM           : SREM key member [...]
    SISMEMBER      : SISMEMBER key member
    SCARD          : SCARD key
    SMEMBERS       : SMEMBERS key
    SPOP           : SPOP key [count]
    SRANDMEMBER    : SRANDMEMBER key [count]
