#include "health.h"
#include "../utils/logger.h"
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>

void init_backend_pool(struct backend_pool *pool)
{
    // 풀 mutex 초기화
    // pthread_mutex_init(&pool->pool_mutex, NULL);
    pool->server_count = MAX_BACKENDS;
    atomic_init(&pool->total_requests, 0);
    atomic_init(&pool->total_failures, 0);
    pool->total_response_time = 0;
    pool->avg_response_time = 0;

    for (int i = 0; i < pool->server_count; i++)
    {
        struct backend_server *server = &pool->servers[i];

        server->address = BACKEND_ADDRESS;
        server->port = BASE_PORT + i;
        atomic_init(&server->is_healthy, true);
        atomic_init(&server->failed_responses, 0);
        atomic_init(&server->current_requests, 0);
        atomic_init(&server->total_requests, 0);
        atomic_init(&server->total_failures, 0);
        server->total_response_time = 0;
        server->avg_response_time = 0;
        server->failure_rate = 0;
    }
}

void cleanup_backend_pool(struct backend_pool *pool)
{
    // 모든 mutex 정리
    // pthread_mutex_lock(&pool->pool_mutex);
    // for (int i = 0; i < pool->server_count; i++)
    // {
    //     pthread_mutex_destroy(&pool->servers[i].server_mutex);
    // }
    // pthread_mutex_unlock(&pool->pool_mutex);
    // pthread_mutex_destroy(&pool->pool_mutex);
}

void track_request_start(struct backend_pool *pool, int server_idx)
{
    struct backend_server *server = &pool->servers[server_idx];

    atomic_fetch_add(&server->current_requests, 1);
    atomic_fetch_add(&server->total_requests, 1);
    atomic_fetch_add(&pool->total_requests, 1);
}

void track_request_end(struct backend_pool *pool, int server_idx, bool success, double response_time)
{
    struct backend_server *server = &pool->servers[server_idx];

    atomic_fetch_sub(&server->current_requests, 1);

    if (!success)
    {
        atomic_fetch_add(&server->total_failures, 1);
        atomic_fetch_add(&pool->total_failures, 1);
    }

    server->total_response_time += response_time;
    server->avg_response_time = server->total_response_time / atomic_load(&server->total_requests);
    server->failure_rate = ((double)atomic_load(&server->total_failures) / atomic_load(&server->total_requests)) * 100;

    pool->total_response_time += response_time;
    pool->avg_response_time = pool->total_response_time / atomic_load(&pool->total_requests);

    update_server_status(pool, server_idx, success);
}

void update_server_status(struct backend_pool *pool, int server_idx, bool request_success)
{
    struct backend_server *server = &pool->servers[server_idx];

    if (!request_success)
    {
        int failed = atomic_fetch_add(&server->failed_responses, 1) + 1;
        if (failed >= MAX_FAILURES)
        {
            atomic_store(&server->is_healthy, false);
        }
    }
    else
    {
        atomic_store(&server->failed_responses, 0);
        atomic_store(&server->is_healthy, true);
    }
}

bool is_server_available(struct backend_pool *pool, int server_idx)
{
    struct backend_server *server = &pool->servers[server_idx];
    return atomic_load(&server->is_healthy);
}