#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <stdbool.h>
#include <netinet/in.h>

// 작업 정의
struct work_item
{
    int client_fd;
    struct sockaddr_in client_addr;
    struct work_item *next;
};

// 작업 큐 정의
struct work_queue
{
    struct work_item *front;
    struct work_item *rear;
    pthread_mutex_t work_mutex;
    pthread_cond_t work_cond;
    int count;
};

struct thread_pool
{
    pthread_t *threads;
    int num_threads;
    struct work_queue queue;
    bool shutdown;
};

int thread_pool_init(struct thread_pool *pool, int num_threads);
void thread_pool_destroy(struct thread_pool *pool);
int thread_pool_add_work(struct thread_pool *pool, int client_fd, struct sockaddr_in client_addr);

#endif