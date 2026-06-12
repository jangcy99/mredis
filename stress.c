#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include "mredis_core.h"

#define MAX_PROCESSES 16
#define ITERATIONS 10000
#define MAX_ALLOC_SIZE 1024

void stress_worker(const char *mredis_name) {
    MRedisHandle *h = mredis_open_existing(mredis_name);
    if (!h) {
        perror("mredis_open_existing failed");
        exit(1);
    }

    uint64_t offsets[100];
    for (int i = 0; i < 100; i++) offsets[i] = OFFSET_NULL;

    srand(getpid() ^ time(NULL));

    for (int i = 0; i < ITERATIONS; i++) {
        int idx = rand() % 100;
        
        if (offsets[idx] == OFFSET_NULL) {
            // 무작위 크기 할당 테스트
            uint64_t size = (rand() % MAX_ALLOC_SIZE) + 1;
            offsets[idx] = heap_alloc(h, size);
        } else {
            // 기존 메모리 해제 테스트
            if (heap_free(h, offsets[idx]) == SHM_OK) {
                offsets[idx] = OFFSET_NULL;
            }
			else	printf ("heap_free failed\n");
        }
    }
    for (int i = 0; i < 100; i++)	{
		if (offsets[i] != OFFSET_NULL)	{
			if (heap_free(h, offsets[i]) != SHM_OK)	{
				printf ("정리:heap_free failed\n");
			}
		}
	}

    mredis_close(h);
    exit(0);
}

int main(__attribute__((unused)) int argc, __attribute__((unused)) char *argv[]) {
    const char *mredis_name = "/mredis_stress_test";
    uint64_t mredis_size = 1024 * 1024 * 1024; // 128MB
	MRedisHandle	*h;
	// 1. 초기 SHM 생성 및 힙 초기화
	mredis_destroy(mredis_name);
	h = mredis_create(mredis_name, mredis_size);
	if (!h) {
		return 1;
	}
	mredis_close(h);

	printf("Starting Stress Test with %d processes...\n", MAX_PROCESSES);

	// 2. 멀티 프로세스 스폰
	pid_t pids[MAX_PROCESSES];
	for (int i = 0; i < MAX_PROCESSES; i++) {
		pids[i] = fork();
		if (pids[i] == 0) stress_worker(mredis_name);
	}

	// 3. 자식 프로세스 대기
	for (int i = 0; i < MAX_PROCESSES; i++) {
		waitpid(pids[i], NULL, 0);
	}

    // 4. 결과 검증
    h = mredis_open_existing(mredis_name);
    mredis_dump_stats(h); // 할당/해제 통계 및 무결성 확인 [cite: 32]
    mredis_close(h);

    printf("Stress Test Completed.\n");
    return 0;
}
