# SHM - Redis-like In-Memory Data Store

**SHM mredis **는 공유 메모리(Shared Memory)를 기반으로 한 고성능 Redis-like 인메모리 데이터 저장소입니다.  
C 언어로 작성되었으며, 멀티프로세스 환경에서도 안전하게 동작합니다.

## 🚀 주요 기능

### 지원하는 데이터 타입
- **String / KV** (`SET`, `GET`, `DEL`, `MSET`, `MGET`)
- **Hash** (`HCREATE`, `HSET`, `HGET`, `HDEL`, `HGETALL`, `HKEYS`, `HVALS`, `HINCRBY`, `HINCRBYFLOAT`)
- **Sorted Set** (`ZCREATE`, `ZADD`, `ZRANGE`, `ZPOPMIN`, `ZCARD`, `ZSCORE` 등)
                  (`BSET`, `BGET`, `BRANGE`, `BRANGEBYSCORE`..)
- **Set** (`SCREATE`, `SADD`, `SREM`, `SMEMBERS`, `SCARD`, `SPOP`, `SRANDMEMBER`, `SISMEMBER`)
- **Pub/Sub** (`PUBLISH`, `SUBSCRIBE`, `UNSUBSCRIBE`)

### 서버 기능
- **RESP 프로토콜** 완벽 지원 (`redis-cli` 바로 사용 가능)
- 멀티스레드 + 멀티프로세스 지원
- 공유 메모리 기반 (재시작 후에도 데이터 유지 가능)

---


---

## 🛠️ 빌드 방법

```bash
# 기본 빌드 (CI 모드)
make

# 서버 빌드
make server

# 전체 테스트 실행
make run

# 서버 실행 (포트 6379)
./shm_server


String

SET, GET, DEL, MSET, MGET

Hash

HCREATE, HDROP, HSET, HGET, HDEL, HEXISTS, HLEN
HGETALL, HKEYS, HVALS, HINCRBY, HINCRBYFLOAT

Sorted Set

ZCREATE, ZDROP, ZADD, ZREM, ZCARD, ZSCORE, ZRANK
ZRANGE, ZRANGEBYSCORE, ZPOPMIN, ZPOPMAX

BDROP, BSET, BREM, BCARD, BSCORE, BRANK
BRANGE, BRANGEBYSCORE, BPOPMIN, BPOPMAX

Set

SCREATE, SDROP, SADD, SREM, SISMEMBER, SCARD
SMEMBERS, SPOP, SRANDMEMBER

# 서버 실행
./shm_server 6379

# 다른 터미널에서 redis-cli로 테스트
redis-cli -p 6379

127.0.0.1:6379> SET name "홍길동"
OK
127.0.0.1:6379> HSET user:1 name "김철수" age 28
(integer) 2
127.0.0.1:6379> SADD myset apple banana cherry
(integer) 3
127.0.0.1:6379> SUBSCRIBE news

./test_all          # 전체 통합 테스트
./test_ci           # CI용 경량 테스트

⚙️ 기술 스택

언어: C99
동기화: pthread mutex (process-shared + robust)
메모리 관리: Best-Fit Bin Heap
프로토콜: RESP (Redis Serialization Protocol)
IPC: POSIX Shared Memory


📌 특징

고성능: 공유 메모리 + 세밀한 락킹
안정성: Robust Mutex, 타입 안전성 검사
메모리 효율: Field Interning (Hash), Best-Fit Heap
Redis 호환: redis-cli로 바로 사용 가능
멀티프로세스: fork 안전 설계
SORTEDSET: ZIPLIST, BINARY KEY BLOCK
📄 라이선스
MIT License

개발자: Claude + Grok + jangcy99
