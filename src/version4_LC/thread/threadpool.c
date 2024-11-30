#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include "threadpool.h"
#include "../proxy/proxy.h"
#include "../utils/logger.h"

// worker 스레드
static void *worker_thread(void *arg)
{
    if (!arg)
    {
        log_message(LOG_ERROR, "Null thread pool argument");
        return NULL;
    } // debug
    struct thread_pool *pool = (struct thread_pool *)arg;
    struct work_item *work;

    while (1)
    {
        // mutex
        pthread_mutex_lock(&pool->queue.work_mutex);

        while (pool->queue.count == 0 && !pool->shutdown)
        {
            // 큐가 비어있는 동안 대기(cond wait을 사용하여 signal을 받을 때 까지 unlock 상태 유지)
            // cond_wait을 사용하여 busy waiting 하지 않음.
            pthread_cond_wait(&pool->queue.work_cond, &pool->queue.work_mutex);
        }

        // 종료신호인 경우
        if (pool->shutdown)
        {
            pthread_mutex_unlock(&pool->queue.work_mutex);
            pthread_exit(NULL);
        }

        // 큐에서 작업 가져오기
        work = pool->queue.front;
        pool->queue.front = work->next;

        // 큐가 빈 경우 rear 처리
        if (pool->queue.front == NULL)
        {
            pool->queue.rear = NULL;
        }
        pool->queue.count--;

        pthread_mutex_unlock(&pool->queue.work_mutex);

        // 실제 작업 처리 (handle_connection)
        handle_connection(work->client_fd, work->client_addr);

        free(work);
    }

    return NULL;
}

int thread_pool_init(struct thread_pool *pool, int num_threads)
{
    if (num_threads <= 0)
    {
        return -1;
    }

    // 스레드 풀 초기화
    pool->threads = malloc(sizeof(pthread_t) * num_threads);
    pool->num_threads = num_threads;
    pool->shutdown = false;

    // 작업 큐 초기화
    pool->queue.front = NULL;
    pool->queue.rear = NULL;
    pool->queue.count = 0;
    pthread_mutex_init(&pool->queue.work_mutex, NULL);
    pthread_cond_init(&pool->queue.work_cond, NULL);

    // worker 스레드 생성
    for (int i = 0; i < num_threads; i++)
    {
        if (pthread_create(&pool->threads[i], NULL, worker_thread, pool) != 0)
        {
            thread_pool_destroy(pool);
            return -1;
        }
    }

    return 0;
}

void thread_pool_destroy(struct thread_pool *pool)
{
    if (pool == NULL)
        return;

    // 스레드 풀 종료 설정
    pthread_mutex_lock(&pool->queue.work_mutex);

    // 종료 siganl
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->queue.work_cond);
    pthread_mutex_unlock(&pool->queue.work_mutex);

    // 모든 스레드 종료 대기
    for (int i = 0; i < pool->num_threads; i++)
    {
        pthread_join(pool->threads[i], NULL);
    }

    // 남은 작업 정리
    struct work_item *work = pool->queue.front;
    while (work != NULL)
    {
        struct work_item *next = work->next;
        close(work->client_fd);
        free(work);
        work = next;
    }

    // 리소스 정리
    pthread_mutex_destroy(&pool->queue.work_mutex);
    pthread_cond_destroy(&pool->queue.work_cond);
    free(pool->threads);
}

// 새로운 작업 추가
int thread_pool_add_work(struct thread_pool *pool, int client_fd, struct sockaddr_in client_addr)
{
    struct work_item *work = malloc(sizeof(struct work_item));
    if (work == NULL)
    {
        log_message(LOG_ERROR, "Failed to allocate work item");
        return -1;
    }
    memset(work, 0, sizeof(struct work_item));

    work->client_fd = client_fd;
    memcpy(&(work->client_addr), &client_addr, sizeof(struct sockaddr_in));
    work->next = NULL;

    // 작업 큐에 추가
    pthread_mutex_lock(&pool->queue.work_mutex);

    if (pool->queue.rear == NULL)
    {
        pool->queue.front = work;
        pool->queue.rear = work;
    }
    else
    {
        pool->queue.rear->next = work;
        pool->queue.rear = work;
    }
    pool->queue.count++;

    pthread_cond_signal(&pool->queue.work_cond);
    pthread_mutex_unlock(&pool->queue.work_mutex);

    return 0;
}
