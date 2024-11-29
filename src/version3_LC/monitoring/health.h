#ifndef HEALTH_H
#define HEALTH_H

#include <stdbool.h>
#include <time.h>

#define MAX_FAILURES 3
#define MAX_BACKENDS 5 // HTTP 서버 최대 개수
#define BASE_PORT 39020
#define BACKEND_ADDRESS "10.198.138.212"

struct backend_server
{
    char *address;
    int port;
    bool is_healthy;
    int failed_responses;

    // 메트릭
    int current_requests;
    int total_requests;
    int total_failures;
    double total_response_time;
    double avg_response_time;
    double failure_rate;
};

struct backend_pool
{
    struct backend_server servers[MAX_BACKENDS];
    int server_count;

    // 전체 시스템 메트릭
    int total_requests;
    int total_failures;
    double total_response_time;
    double avg_response_time;
};

void init_backend_pool(struct backend_pool *pool);
void cleanup_backend_pool(struct backend_pool *pool);
void track_request_start(struct backend_pool *pool, int server_idx);
void track_request_end(struct backend_pool *pool, int server_idx, bool success, double response_time);
void update_server_status(struct backend_pool *pool, int server_idx, bool request_success);
bool is_server_available(struct backend_pool *pool, int server_idx);

#endif