#ifndef METRICS_H
#define METRICS_H

struct server_metrics
{
    // 서버별 메트릭
    int current_requests;       // 현재 활성 요청 수
    int total_requests;         // 총 처리 요청 수
    int failed_requests;        // 실패한 요청 수
    double total_response_time; // 총 응답 시간 (평균 계산용)
    double avg_response_time;   // 평균 응답 시간
    double failure_rate;        // 실패율 (%)
};

struct system_metrics
{
    // 전체 시스템 메트릭
    int total_throughput;      // 총 처리량
    int total_errors;          // 총 에러 수
    double error_rate;         // 전체 에러율
    double load_balance_score; // 부하 분산 상태 점수 (0-1)
};

// 메트릭 초기화
void init_server_metrics(struct server_metrics *metrics);
void init_system_metrics(struct system_metrics *metrics);

// 메트릭 업데이트
void update_request_start(struct server_metrics *metrics);
void update_request_end(struct server_metrics *metrics, bool success, double response_time);
void update_system_metrics(struct system_metrics *metrics, struct server_metrics *server_metrics, int server_count);

// 메트릭 계산
void calculate_server_metrics(struct server_metrics *metrics);
void calculate_system_metrics(struct system_metrics *metrics);

#endif