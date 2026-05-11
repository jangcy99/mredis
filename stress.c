#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include "shm_core.h"

#define MAX_PROCESSES 16
#define ITERATIONS 10000
#define MAX_ALLOC_SIZE 1024

void stress_worker(const char *shm_name) {
    ShmHandle *h = shm_open_existing(shm_name);
    if (!h) {
        perror("shm_open_existing failed");
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

    shm_close(h);
    exit(0);
}

int main(__attribute__((unused)) int argc, __attribute__((unused)) char *argv[]) {
    const char *shm_name = "/shm_stress_test";
    uint64_t shm_size = 1024 * 1024 * 1024; // 128MB
	ShmHandle	*h;
	// 1. 초기 SHM 생성 및 힙 초기화
	shm_destroy(shm_name);
	h = shm_create(shm_name, shm_size);
	if (!h) {
		return 1;
	}
	shm_close(h);

	printf("Starting Stress Test with %d processes...\n", MAX_PROCESSES);

	// 2. 멀티 프로세스 스폰
	pid_t pids[MAX_PROCESSES];
	for (int i = 0; i < MAX_PROCESSES; i++) {
		pids[i] = fork();
		if (pids[i] == 0) stress_worker(shm_name);
	}

	// 3. 자식 프로세스 대기
	for (int i = 0; i < MAX_PROCESSES; i++) {
		waitpid(pids[i], NULL, 0);
	}

    // 4. 결과 검증
    h = shm_open_existing(shm_name);
    shm_dump_stats(h); // 할당/해제 통계 및 무결성 확인 [cite: 32]
    shm_close(h);

    printf("Stress Test Completed.\n");
    return 0;
}
