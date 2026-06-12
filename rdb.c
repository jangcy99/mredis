/*
 * rdb.c  –  RDB Snapshot 구현
 *
 *  설계 원칙
 *  ──────────────────────────────────────────────────────────────────
 *  1. 기존 파일 무수정. mredis_types.h / mredis_core.h 공개 API 만 사용.
 *  2. tmp 파일에 쓰고 완료 후 rename → 저장 중 크래시로 파일 손상 없음.
 *  3. 파일 끝에 CRC-32 → 적재 시 무결성 자동 검증.
 *  4. BGSAVE: fork() → 자식이 SHM CoW 스냅샷 저장.
 *  5. 자료구조 직접 순회 (mutex trylock) → 데드락 없음.
 *  6. key_count: 버킷 두 번 순회 회피를 위해 헤더를 마지막에 pwrite.
 *     CRC는 헤더의 key_count 를 제외한 나머지 필드만으로 계산하지
 *     않고, key_count 포함 전체 헤더를 CRC 에 포함시키되
 *     저장 순서를 "먼저 1회 순회해 key_count 확정 → 헤더 기록 →
 *     본문 기록" 순으로 처리한다.
 *
 *  on-disk 직렬화 레이아웃
 *  ──────────────────────────────────────────────────────────────────
 *  [RdbFileHeader 36B]
 *  반복 레코드:
 *    [uint8_t  type ]
 *    [uint32_t klen ]
 *    [char     key[klen]]
 *    payload:
 *      KV  : [uint32_t vlen][char val[vlen]]
 *      ZSET: [uint64_t cnt] [cnt × (double score, uint32_t mlen, char m[mlen])]
 *      HASH: [uint64_t cnt] [cnt × (uint32_t flen, char f[flen],
 *                                   uint32_t vlen, char v[vlen])]
 *  [uint8_t ENTRY_EOF]
 *  [uint32_t crc32]   ← 헤더부터 EOF 바이트까지 전체 CRC
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <pthread.h>

#include "mredis_types.h"
#include "mredis_core.h"
#include "cmd_dispatch.h"
#include "rdb.h"

/* ============================================================
 *  §1  CRC-32 (IEEE 802.3)
 * ============================================================ */
static const uint32_t crc32_table[256] = {
    0x00000000,0x77073096,0xEE0E612C,0x990951BA,0x076DC419,0x706AF48F,
    0xE963A535,0x9E6495A3,0x0EDB8832,0x79DCB8A4,0xE0D5E91B,0x97D2D988,
    0x09B64C2B,0x7EB17CBF,0xE7B82D09,0x90BF1D3D,0x1DB71064,0x6AB020F2,
    0xF3B97148,0x84BE41DE,0x1ADAD47D,0x6DDDE4EB,0xF4D4B551,0x83D385C7,
    0x136C9856,0x646BA8C0,0xFD62F97A,0x8A65C9EC,0x14015C4F,0x63066CD9,
    0xFA0F3D63,0x8D080DF5,0x3B6E20C8,0x4C69105E,0xD56041E4,0xA2677172,
    0x3C03E4D1,0x4B04D447,0xD20D85FD,0xA50AB56B,0x35B5A8FA,0x42B2986C,
    0xDBBBC9D6,0xACBCF940,0x32D86CE3,0x45DF5C75,0xDCD60DCF,0xABD13D59,
    0x26D930AC,0x51DE003A,0xC8D75180,0xBFD06116,0x21B4F4B5,0x56B3C423,
    0xCFBA9599,0xB8BDA50F,0x2802B89E,0x5F058808,0xC60CD9B2,0xB10BE924,
    0x2F6F7C87,0x58684C11,0xC1611DAB,0xB6662D3D,0x76DC4190,0x01DB7106,
    0x98D220BC,0xEFD5102A,0x71B18589,0x06B6B51F,0x9FBFE4A5,0xE8B8D433,
    0x7807C9A2,0x0F00F934,0x9609A88E,0xE10E9818,0x7F6A0DBB,0x086D3D2D,
    0x91646C97,0xE6635C01,0x6B6B51F4,0x1C6C6162,0x856530D8,0xF262004E,
    0x6C0695ED,0x1B01A57B,0x8208F4C1,0xF50FC457,0x65B0D9C6,0x12B7E950,
    0x8BBEB8EA,0xFCB9887C,0x62DD1D7F,0x15DA2D49,0x8CD37CF3,0xFBD44C65,
    0x4DB26158,0x3AB551CE,0xA3BC0074,0xD4BB30E2,0x4ADFA541,0x3DD895D7,
    0xA4D1C46D,0xD3D6F4FB,0x4369E96A,0x346ED9FC,0xAD678846,0xDA60B8D0,
    0x44042D73,0x33031DE5,0xAA0A4C5F,0xDD0D7CC9,0x5005713C,0x270241AA,
    0xBE0B1010,0xC90C2086,0x5768B525,0x206F85B3,0xB966D409,0xCE61E49F,
    0x5EDEF90E,0x29D9C998,0xB0D09822,0xC7D7A8B4,0x59B33D17,0x2EB40D81,
    0xB7BD5C3B,0xC0BA6CAD,0xEDB88320,0x9ABFB3B6,0x03B6E20C,0x74B1D29A,
    0xEAD54739,0x9DD277AF,0x04DB2615,0x73DC1683,0xE3630B12,0x94643B84,
    0x0D6D6A3E,0x7A6A5AA8,0xE40ECF0B,0x9309FF9D,0x0A00AE27,0x7D079EB1,
    0xF00F9344,0x8708A3D2,0x1E01F268,0x6906C2FE,0xF762575D,0x806567CB,
    0x196C3671,0x6E6B06E7,0xFED41B76,0x89D32BE0,0x10DA7A5A,0x67DD4ACC,
    0xF9B9DF6F,0x8EBEEFF9,0x17B7BE43,0x60B08ED5,0xD6D6A3E8,0xA1D1937E,
    0x38D8C2C4,0x4FDFF252,0xD1BB67F1,0xA6BC5767,0x3FB506DD,0x48B2364B,
    0xD80D2BDA,0xAF0A1B4C,0x36034AF6,0x41047A60,0xDF60EFC3,0xA8670955,
    0x316658EF,0x465D6879,0xB40BBE37,0xC30C8EA1,0x5A05DF1B,0x2D02EF8D
};

uint32_t rdb_crc32(const void *data, size_t len, uint32_t crc)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) crc = crc32_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ============================================================
 *  §2  직렬화 Writer (fd 기반, CRC 누적)
 * ============================================================ */
typedef struct {
    int      fd;
    uint32_t crc;
    int      err;
    uint64_t bytes;
} RdbWriter;

static void rw_init(RdbWriter *w, int fd)
    { w->fd=fd; w->crc=0; w->err=0; w->bytes=0; }

static void rw_raw(RdbWriter *w, const void *buf, size_t n)
{
    if (w->err || n==0) return;
    w->crc = rdb_crc32(buf, n, w->crc);
    ssize_t written = write(w->fd, buf, n);
    if (written<0 || (size_t)written!=n) {
        fprintf(stderr,"RDB: write 오류: %s\n",strerror(errno));
        w->err=-1;
    } else {
        w->bytes += (uint64_t)written;
    }
}

#define RW_U8(w,v)    do{uint8_t  _v=(uint8_t)(v); rw_raw(w,&_v,1);}while(0)
#define RW_U32(w,v)   do{uint32_t _v=(uint32_t)(v);rw_raw(w,&_v,4);}while(0)
#define RW_U64(w,v)   do{uint64_t _v=(uint64_t)(v);rw_raw(w,&_v,8);}while(0)
#define RW_DBL(w,v)   do{double   _v=(double)(v);  rw_raw(w,&_v,sizeof(double));}while(0)
#define RW_STR(w,p,n) do{RW_U32(w,n); rw_raw(w,p,n);}while(0)

/* ============================================================
 *  §3  직렬화 Reader (FILE* 기반, CRC 누적)
 * ============================================================ */
typedef struct {
    FILE    *fp;
    uint32_t crc;
    int      err;
} RdbReader;

static void rr_init(RdbReader *r, FILE *fp)
    { r->fp=fp; r->crc=0; r->err=0; }

static void rr_raw(RdbReader *r, void *buf, size_t n)
{
    if (r->err || n==0) return;
    if (fread(buf,1,n,r->fp) != n) {
        if (!feof(r->fp))
            fprintf(stderr,"RDB: read 오류: %s\n",strerror(errno));
        r->err=-1;
        return;
    }
    r->crc = rdb_crc32(buf, n, r->crc);
}

static int rr_raw_nocrc(RdbReader *r, void *buf, size_t n)
    { return fread(buf,1,n,r->fp)==n ? 0 : -1; }

static uint8_t  rr_u8 (RdbReader *r){uint8_t  v=0;rr_raw(r,&v,1);       return v;}
static uint32_t rr_u32(RdbReader *r){uint32_t v=0;rr_raw(r,&v,4);       return v;}
static uint64_t rr_u64(RdbReader *r){uint64_t v=0;rr_raw(r,&v,8);       return v;}
static double   rr_dbl(RdbReader *r){double   v=0;rr_raw(r,&v,sizeof(v));return v;}

static char *rr_str(RdbReader *r, uint32_t *out_len)
{
    uint32_t len=rr_u32(r);
    if (r->err) return NULL;
    char *buf=(char*)malloc(len+1);
    if (!buf){r->err=-1;return NULL;}
    rr_raw(r,buf,len);
    buf[len]='\0';
    *out_len=len;
    return buf;
}

/* ============================================================
 *  §4  버킷 카운트 1회 순회 (key_count 확정용)
 * ============================================================ */
static uint64_t count_keys(MRedisHandle *shm)
{
    MRedisHeader *shdr = core_mredis_hdr(shm);
    uint64_t   cnt  = 0;
    for (uint32_t i=0; i<shdr->hash_table_size; i++) {
        BucketEntry *bk = core_get_bucket(shm, i);
        if (pthread_mutex_trylock(&bk->mutex) != 0) continue;
        uint64_t cur = bk->head_offset;
        while (cur != OFFSET_NULL) {
            NameEntry *ne = (NameEntry*)OFF2PTR(shm,cur);
            if (ne->type==ENTRY_KV||ne->type==ENTRY_ZSET||ne->type==ENTRY_HASH)
                cnt++;
            cur = ne->next_offset;
        }
        pthread_mutex_unlock(&bk->mutex);
    }
    return cnt;
}

/* ============================================================
 *  §5  레코드 직렬화
 * ============================================================ */
static void save_kv(MRedisHandle *h, RdbWriter *w,
                    const char *key, uint32_t klen, NameEntry *ne)
{
    KVNode *kv = (KVNode*)OFF2PTR(h, ne->data_offset);
    RW_U8 (w, ENTRY_KV);
    RW_STR(w, key, klen);
    if (kv->val_len>0)
        RW_STR(w,(const char*)OFF2PTR(h,kv->val_offset),kv->val_len);
    else
        RW_STR(w,"",0);
}

static void save_zset(MRedisHandle *h, RdbWriter *w,
                      const char *key, uint32_t klen, NameEntry *ne)
{
    ZSetHeader *zsh = (ZSetHeader*)OFF2PTR(h, ne->data_offset);
    RW_U8 (w, ENTRY_ZSET);
    RW_STR(w, key, klen);
    RW_U64(w, zsh->length);
    uint64_t cur = core_sn(h, zsh->head_offset)->forward[0];
    while (cur != OFFSET_NULL) {
        SkipNode *sn = core_sn(h,cur);
        RW_DBL(w, sn->score);
        RW_STR(w,(const char*)OFF2PTR(h,sn->member_offset),sn->member_len);
        cur = sn->forward[0];
    }
}

static void save_hash(MRedisHandle *h, RdbWriter *w,
                      const char *key, uint32_t klen, NameEntry *ne)
{
    HashHeader *hh   = (HashHeader*)OFF2PTR(h, ne->data_offset);
    uint64_t   *bkts = hh_field_buckets(hh);
    RW_U8 (w, ENTRY_HASH);
    RW_STR(w, key, klen);
    RW_U64(w, hh->field_count);
    for (uint32_t i=0; i<hh->n_buckets; i++) {
        uint64_t cur = bkts[i];
        while (cur != OFFSET_NULL) {
            FieldEntry *fe = (FieldEntry*)OFF2PTR(h,cur);
            RW_STR(w,(const char*)OFF2PTR(h,fe->field_offset),fe->field_len);
            if (fe->val_len>0)
                RW_STR(w,(const char*)OFF2PTR(h,fe->val_offset),fe->val_len);
            else
                RW_STR(w,"",0);
            cur = fe->next_offset;
        }
    }
}

/* rdb.c 에 추가할 함수들 */

#include "cmd_bset.h"
/* BSET 저장 */
static void save_bset(MRedisHandle *h, RdbWriter *w,
                      const char *key, uint32_t klen, NameEntry *ne)
{
    BSetHeader *bsh = (BSetHeader*)OFF2PTR(h, ne->data_offset);
    if (!bsh) return;

    pthread_rwlock_rdlock(&bsh->rwlock);

    RW_U8(w, ENTRY_BSET);
    RW_STR(w, key, klen);
    RW_U64(w, bsh->count);

    BSetEntry *entries = (BSetEntry*)OFF2PTR(h, bsh->array_offset);

    for (uint64_t i = 0; i < bsh->count; i++) {
        RW_U64(w, entries[i].score);
        RW_STR(w, (const char*)OFF2PTR(h, entries[i].offset), entries[i].vlen);
    }

    pthread_rwlock_unlock(&bsh->rwlock);
}

/* BSET 복구 */
static int load_bset(RdbReader *r, MRedisHandle *h,
                     const char *key, uint32_t klen)
{
    uint64_t count = rr_u64(r);
    if (r->err) return -1;

    /* BSET 생성 */
    string_t skey = {(char*)key, klen};
    string_t cmd  = {"BCREATE", 7};
    string_t *args[2] = {&cmd, &skey};
    s_replyObject *rep = cmd_dispatch(h, args, 2);
    reply_free(rep);

    for (uint64_t i = 0; i < count; i++) {
        uint64_t score = rr_u64(r);
        uint32_t vlen;
        char *value = rr_str(r, &vlen);
        if (r->err || !value) {
            free(value);
            return -1;
        }

        char score_str[32];
        snprintf(score_str, sizeof(score_str), "%llu", (unsigned long long)score);

        string_t scmd = {"BSET", 4};
        string_t sscore = {score_str, (uint32_t)strlen(score_str)};
        string_t svalue = {value, vlen};

        string_t *set_args[4] = {&scmd, &skey, &sscore, &svalue};
        rep = cmd_dispatch(h, set_args, 4);
        reply_free(rep);
        free(value);
    }
    return 0;
}
/* ============================================================
 *  §6  rdb_save (포그라운드 저장)
 *
 *  순서:
 *    (1) key_count 먼저 산출 (버킷 1회 순회)
 *    (2) 헤더 완성 후 기록          → CRC 에 정확한 key_count 포함
 *    (3) 레코드 직렬화 기록
 *    (4) EOF 마커 기록
 *    (5) CRC 기록 (CRC 범위 밖)
 *    (6) fdatasync + atomic rename
 * ============================================================ */
int rdb_save(RdbHandle *rdb, MRedisHandle *shm)
{
    if (!rdb || !shm) return SHM_ERR;

    /* tmp 경로 */
    char tmp[512];
    snprintf(tmp,sizeof(tmp),"%s%s",rdb->path,RDB_TMP_SUFFIX);

    int fd = open(tmp, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd<0) {
        fprintf(stderr,"RDB: open(%s) 실패: %s\n",tmp,strerror(errno));
        return SHM_ERR;
    }

    /* (1) key_count 먼저 확정 */
    uint64_t key_count = count_keys(shm);

    /* (2) 헤더 완성 후 기록 */
    RdbFileHeader fhdr;
    memset(&fhdr,0,sizeof(fhdr));
    memcpy(fhdr.magic, RDB_MAGIC, RDB_MAGIC_LEN);
    fhdr.version    = RDB_VERSION;
    fhdr.created_at = (uint64_t)time(NULL);
    fhdr.key_count  = key_count;   /* ← 정확한 값 */

    RdbWriter w;
    rw_init(&w, fd);
    rw_raw(&w, &fhdr, sizeof(fhdr));   /* CRC 에 완전한 헤더 포함 */

    /* (3) 레코드 직렬화 */
    MRedisHeader *shdr = core_mredis_hdr(shm);
    for (uint32_t i=0; i<shdr->hash_table_size; i++) {
        BucketEntry *bk = core_get_bucket(shm,i);
        if (pthread_mutex_trylock(&bk->mutex) != 0) continue;
        uint64_t cur = bk->head_offset;
        while (cur!=OFFSET_NULL && !w.err) {
            NameEntry  *ne  = (NameEntry*)OFF2PTR(shm,cur);
            const char *key = (const char*)OFF2PTR(shm,ne->key_offset);
            uint32_t    klen = ne->key_len;
            switch (ne->type) {
            case ENTRY_KV:   save_kv  (shm,&w,key,klen,ne); break;
            case ENTRY_ZSET: save_zset(shm,&w,key,klen,ne); break;
            case ENTRY_HASH: save_hash(shm,&w,key,klen,ne); break;
            case ENTRY_BSET: save_bset(shm,&w,key,klen,ne); break;
            default: break;
            }
            cur = ne->next_offset;
        }
        pthread_mutex_unlock(&bk->mutex);
        if (w.err) break;
    }

    /* (4) EOF 마커 */
    RW_U8(&w, ENTRY_EOF);

    if (w.err) {
        close(fd); unlink(tmp);
        fprintf(stderr,"RDB: 직렬화 오류\n");
        return SHM_ERR;
    }

    /* (5) CRC (CRC 계산 범위 밖 → rw_raw 사용 안 함) */
    uint32_t final_crc = w.crc;
    if (write(fd,&final_crc,4) != 4) {
        close(fd); unlink(tmp); return SHM_ERR;
    }

    fdatasync(fd);
    close(fd);

    /* (6) atomic rename */
    if (rename(tmp, rdb->path)<0) {
        fprintf(stderr,"RDB: rename 실패: %s\n",strerror(errno));
        unlink(tmp); return SHM_ERR;
    }

    /* 통계 */
    struct stat st;
    rdb->last_save   = time(NULL);
    rdb->last_status = 0;
    rdb->save_count++;
    rdb->key_count   = key_count;
    rdb->file_size   = (stat(rdb->path,&st)==0)?(uint64_t)st.st_size:0;

    fprintf(stderr,"RDB: 저장 완료 – 키 %llu 개, %llu bytes → %s\n",
            (unsigned long long)key_count,
            (unsigned long long)(w.bytes+4),
            rdb->path);
    return SHM_OK;
}

/* ============================================================
 *  §7  레코드 역직렬화 (Reader → SHM)
 * ============================================================ */
static int load_kv(RdbReader *r, MRedisHandle *shm,
                   const char *key, uint32_t klen)
{
    uint32_t vlen; char *val=rr_str(r,&vlen);
    if (r->err||!val){free(val);return -1;}
    string_t sk={key,klen},sv={val,vlen},scmd={"SET",3};
    string_t *args[]={&scmd,&sk,&sv};
    s_replyObject *rep=cmd_dispatch(shm,args,3);
    reply_free(rep);
    free(val);
    return r->err?-1:0;
}

static int load_zset(RdbReader *r, MRedisHandle *shm,
                     const char *key, uint32_t klen)
{
    uint64_t cnt=rr_u64(r);
    if (r->err) return -1;

    /* ZCREATE key */
    string_t sk={key,klen},scrt={"ZCREATE",7};
    string_t *cargs[]={&scrt,&sk};
    s_replyObject *rep=cmd_dispatch(shm,cargs,2);
    reply_free(rep);

    char sc_buf[64];
    for (uint64_t i=0;i<cnt;i++) {
        double   sc =rr_dbl(r);
        uint32_t mlen; char *mem=rr_str(r,&mlen);
        if (r->err||!mem){free(mem);return -1;}
        int sl=snprintf(sc_buf,sizeof(sc_buf),"%.17g",sc);
        string_t ssc={sc_buf,(uint32_t)(sl>0?sl:0)},sm={mem,mlen};
        string_t scmd={"ZADD",4};
        string_t *args[]={&scmd,&sk,&ssc,&sm};
        rep=cmd_dispatch(shm,args,4);
        reply_free(rep);
        free(mem);
    }
    return r->err?-1:0;
}

static int load_hash(RdbReader *r, MRedisHandle *shm,
                     const char *key, uint32_t klen)
{
    uint64_t cnt=rr_u64(r);
    if (r->err) return -1;

    /* HCREATE key */
    string_t sk={key,klen},scrt={"HCREATE",7};
    string_t *cargs[]={&scrt,&sk};
    s_replyObject *rep=cmd_dispatch(shm,cargs,2);
    reply_free(rep);

    for (uint64_t i=0;i<cnt;i++) {
        uint32_t flen; char *field=rr_str(r,&flen);
        uint32_t vlen; char *val  =rr_str(r,&vlen);
        if (r->err||!field||!val){free(field);free(val);return -1;}
        string_t sf={field,flen},sv={val,vlen};
        string_t scmd={"HSET",4};
        string_t *args[]={&scmd,&sk,&sf,&sv};
        rep=cmd_dispatch(shm,args,4);
        reply_free(rep);
        free(field);free(val);
    }
    return r->err?-1:0;
}


/* ============================================================
 *  §8  rdb_load
 * ============================================================ */
int64_t rdb_load(RdbHandle *rdb, MRedisHandle *shm)
{
    if (!rdb||!shm) return -1;

    FILE *fp=fopen(rdb->path,"rb");
    if (!fp) {
        if (errno==ENOENT) {
            fprintf(stderr,"RDB: 파일 없음 – 새 데이터베이스 시작\n");
            return 0;
        }
        fprintf(stderr,"RDB: fopen 실패: %s\n",strerror(errno));
        return -1;
    }

    struct stat st;
    fstat(fileno(fp),&st);
    if ((uint64_t)st.st_size < sizeof(RdbFileHeader)+5) {
        fprintf(stderr,"RDB: 파일 너무 작음\n");
        fclose(fp); return -1;
    }

    RdbReader rr;
    rr_init(&rr,fp);

    /* 헤더 */
    RdbFileHeader fhdr;
    rr_raw(&rr,&fhdr,sizeof(fhdr));
    if (rr.err){fclose(fp);return -1;}

    if (memcmp(fhdr.magic,RDB_MAGIC,RDB_MAGIC_LEN)!=0) {
        fprintf(stderr,"RDB: 잘못된 매직\n");
        fclose(fp); return -1;
    }
    if (fhdr.version > RDB_VERSION) {
        fprintf(stderr,"RDB: 지원하지 않는 버전 %u\n",fhdr.version);
        fclose(fp); return -1;
    }
    {
        time_t _ct=(time_t)fhdr.created_at;
        char ts[64]; struct tm *tm=localtime(&_ct);
        strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",tm);
        fprintf(stderr,"RDB: 적재 시작 (ver=%u, keys=%llu, created=%s)\n",
                fhdr.version,(unsigned long long)fhdr.key_count,ts);
    }

    /* 레코드 루프 */
    int64_t loaded=0;
    while (!rr.err) {
        uint8_t type=rr_u8(&rr);
        if (rr.err) break;
        if (type==ENTRY_EOF) break;

        uint32_t klen; char *key=rr_str(&rr,&klen);
        if (rr.err||!key){free(key);break;}

        int rc=0;
        switch (type) {
        case ENTRY_KV:   rc=load_kv  (&rr,shm,key,klen);break;
        case ENTRY_ZSET: rc=load_zset(&rr,shm,key,klen);break;
        case ENTRY_HASH: rc=load_hash(&rr,shm,key,klen);break;
        case ENTRY_BSET:  rc=load_bset(&rr,shm,key,klen);break;
        default:
            fprintf(stderr,"RDB: 알 수 없는 타입 0x%02x\n",type);
            free(key);fclose(fp);return loaded;
        }
        free(key);
        if (rc<0) break;
        loaded++;
    }

    /* CRC 검증 */
    uint32_t stored_crc=0;
    if (rr_raw_nocrc(&rr,&stored_crc,4)==0) {
        if (stored_crc!=rr.crc)
            fprintf(stderr,"RDB: CRC 불일치! stored=0x%08x calc=0x%08x\n",
                    stored_crc,rr.crc);
        else
            fprintf(stderr,"RDB: CRC 검증 통과 (0x%08x)\n",stored_crc);
    }

    fclose(fp);
    fprintf(stderr,"RDB: 적재 완료 – 키 %lld 개\n",(long long)loaded);
    return loaded;
}

/* ============================================================
 *  §9  BGSAVE
 * ============================================================ */
int rdb_save_bg(RdbHandle *rdb, MRedisHandle *shm)
{
    if (!rdb||!shm) return SHM_ERR;
    if (rdb->saving) {
        fprintf(stderr,"RDB: BGSAVE 이미 진행 중\n");
        return SHM_ERR;
    }
    pid_t pid=fork();
    if (pid<0) {
        fprintf(stderr,"RDB: fork 실패: %s\n",strerror(errno));
        return SHM_ERR;
    }
    if (pid==0) {
        int rc=rdb_save(rdb,shm);
        exit(rc==SHM_OK?0:1);
    }
    rdb->bg_child=pid;
    rdb->saving  =1;
    fprintf(stderr,"RDB: BGSAVE 시작 (pid=%d)\n",pid);
    return SHM_OK;
}

int rdb_save_check(RdbHandle *rdb)
{
    if (!rdb||!rdb->saving||rdb->bg_child<0) return 0;
    int status=0;
    pid_t ret=waitpid(rdb->bg_child,&status,WNOHANG);
    if (ret==0) return 0;
    if (ret<0) {
        fprintf(stderr,"RDB: waitpid 오류: %s\n",strerror(errno));
        rdb->saving=0;rdb->bg_child=-1;
        return -1;
    }
    rdb->saving=0;rdb->bg_child=-1;
    if (WIFEXITED(status)&&WEXITSTATUS(status)==0) {
        rdb->last_save  =time(NULL);
        rdb->last_status=0;
        rdb->save_count++;
        struct stat st;
        rdb->file_size=(stat(rdb->path,&st)==0)?(uint64_t)st.st_size:0;
        fprintf(stderr,"RDB: BGSAVE 완료\n");
        return 1;
    }
    rdb->last_status=-1;
    fprintf(stderr,"RDB: BGSAVE 실패 (exit=%d)\n",WEXITSTATUS(status));
    return -1;
}

/* ============================================================
 *  §10  생명주기 / 통계
 * ============================================================ */
RdbHandle *rdb_open(const char *path)
{
    if (!path) path=RDB_DEFAULT_PATH;
    RdbHandle *rdb=(RdbHandle*)calloc(1,sizeof(RdbHandle));
    if (!rdb) return NULL;
    rdb->path     =strdup(path);
    rdb->enabled  =1;
    rdb->bg_child =-1;
    rdb->last_save=0;
    if (!rdb->path){free(rdb);return NULL;}
    fprintf(stderr,"RDB: 핸들 생성 path=%s\n",rdb->path);
    return rdb;
}

void rdb_close(RdbHandle *rdb)
{
    if (!rdb) return;
    free(rdb->path);
    free(rdb);
}

void rdb_stats(RdbHandle *rdb, char *buf, size_t blen)
{
    if (!rdb){snprintf(buf,blen,"RDB: 비활성\n");return;}
    char ts[64]="-";
    if (rdb->last_save>0) {
        time_t t=(time_t)rdb->last_save;
        strftime(ts,sizeof(ts),"%Y-%m-%d %H:%M:%S",localtime(&t));
    }
    snprintf(buf,blen,
             "RDB 통계\n"
             "  경로       : %s\n"
             "  저장 횟수  : %llu\n"
             "  마지막 저장: %s\n"
             "  마지막 상태: %s\n"
             "  키 수      : %llu\n"
             "  파일 크기  : %llu bytes\n"
             "  BGSAVE     : %s\n",
             rdb->path,
             (unsigned long long)rdb->save_count,
             ts,
             rdb->last_status==0?"OK":"FAIL",
             (unsigned long long)rdb->key_count,
             (unsigned long long)rdb->file_size,
             rdb->saving?"진행중":"없음");
}
