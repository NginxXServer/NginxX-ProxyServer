// health.h
#ifndef HEALTH_H
#define HEALTH_H

#include <stdbool.h>
#include <time.h>

#define MAX_FAILURES 3
#define MAX_BACKENDS 5  // 백엔드 서버 최대 개수
#define BASE_PORT 39020 // HTTP 서버 포트
#define BACKEND_ADDRESS "10.198.138.212"

struct backend_server
{
    char *address;
    int port;
    bool is_healthy;
    int failed_responses; // 연속 실패 횟수

    // 메트릭
    int current_requests;       // 현재 처리중인 요청 수
    int total_requests;         // 총 처리된 요청 수
    int total_failures;         // 총 실패 횟수
    double total_response_time; // 총 응답 시간
    double avg_response_time;   // 평균 응답 시간
    double failure_rate;        // 실패율 (%)
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
void track_request_start(struct backend_pool *pool, int server_idx);
void track_request_end(struct backend_pool *pool, int server_idx, bool success, double response_time);
void update_server_status(struct backend_pool *pool, int server_idx, bool request_success);
bool is_server_available(struct backend_pool *pool, int server_idx);

#endif