#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include "health.h"
#include "../utils/logger.h"

#define BUFFER_SIZE 9999

static struct backend_pool pool;
static pthread_mutex_t server_select_mutex = PTHREAD_MUTEX_INITIALIZER;
static int current_server = 0;

// 스레드간 클라이언트 연결 정보 전달을 위한 구조체
struct connection_info
{
    int client_socket;
    struct sockaddr_in client_addr;
};

/**
 * HTTP 서버 선택 함수 (라운드 로빈 방식)
 * - thread-safe를 위해 mutex 사용
 *
 * 반환값:
 * - 성공: 선택된 서버의 인덱스
 * - 실패: -1
 */
int select_server()
{
    int selected = -1;

    // 서버 구성 확인
    if (MAX_BACKENDS <= 0)
    {
        log_message(LOG_ERROR, "No backend servers configured");
        return -1;
    }

    // Mutex 잠금을 시도하고 실패 시 로그 출력 후 반환
    if (pthread_mutex_lock(&server_select_mutex) != 0)
    {
        log_message(LOG_ERROR, "Failed to lock server select mutex");
        return -1;
    }

    // 현재 서버 인덱스 선택
    selected = current_server;
    current_server = (current_server + 1) % MAX_BACKENDS;

    // Mutex 잠금 해제
    if (pthread_mutex_unlock(&server_select_mutex) != 0)
    {
        log_message(LOG_ERROR, "Failed to unlock server select mutex");
        return -1;
    }

    // 서버 정보 로그 출력
    if (pool.servers[selected].address != NULL && pool.servers[selected].port > 0)
    {
        log_message(LOG_INFO, "Selected backend server %s:%d",
                    pool.servers[selected].address,
                    pool.servers[selected].port);
    }
    else
    {
        log_message(LOG_ERROR, "Invalid server configuration at index %d", selected);
        return -1;
    }

    return selected;
}

/**
 * 클라이언트 요청을 처리하는 스레드 함수
 *
 * 매개변수:
 * - arg: connection_info 구조체 포인터 (클라이언트 연결 정보)
 */
void *handle_client(void *arg)
{
    // 연결 정보 추출
    struct connection_info *conn_info = (struct connection_info *)arg;
    int client_socket = conn_info->client_socket;
    struct sockaddr_in client_addr = conn_info->client_addr;
    char buffer[BUFFER_SIZE];
    int bytes_received;

    char *client_ip = inet_ntoa(client_addr.sin_addr);
    log_message(LOG_INFO, "Handling connection from %s in new thread", client_ip);

    // 백엔드 서버 선택 (health check 포함)
    int server_idx = select_server();
    if (server_idx < 0)
    {
        close(client_socket);
        free(conn_info);
        return NULL;
    }

    struct backend_server *server = &pool.servers[server_idx];
    int target_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (target_socket < 0)
    {
        log_message(LOG_ERROR, "Failed to create socket for backend connection");
        close(client_socket);
        free(conn_info);
        return NULL;
    }

    // 요청 처리 시작 시간 기록
    struct timespec start_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    // 요청 시작 추적
    track_request_start(&pool, server_idx);

    // 백엔드 서버 연결 설정
    struct sockaddr_in target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(server->port);
    target_addr.sin_addr.s_addr = inet_addr(server->address);

    bool request_success = true;
    if (connect(target_socket, (struct sockaddr *)&target_addr, sizeof(target_addr)) < 0)
    {
        log_message(LOG_ERROR, "Failed to connect to backend %s:%d",
                    server->address, server->port);
        request_success = false;
    }
    else
    {
        // 클라이언트 -> 백엔드 요청 전달
        bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
        if (bytes_received > 0)
        {
            buffer[bytes_received] = '\0';
            if (send(target_socket, buffer, bytes_received, 0) < 0)
            {
                log_message(LOG_ERROR, "Failed to send data to backend");
                request_success = false;
            }
        }
        else if (bytes_received < 0)
        {
            log_message(LOG_ERROR, "Failed to receive data from client");
            request_success = false;
        }

        // 백엔드 -> 클라이언트 응답 전달 부분을 다음과 같이 수정
        int total_received = 0;

        while ((bytes_received = recv(target_socket, buffer + total_received,
                                      BUFFER_SIZE - total_received - 1, 0)) > 0)
        {
            total_received += bytes_received;
            buffer[total_received] = '\0';

            // 클라이언트에게 전송
            if (send(client_socket, buffer + (total_received - bytes_received), bytes_received, 0) < 0)
            {
                log_message(LOG_ERROR, "Failed to send response to client");
                request_success = false;
                break;
            }
        }
    }

    // 요청 처리 종료 시간 계산
    struct timespec end_time;
    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double response_time = (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                           (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;

    // 요청 종료 추적, 서버 상태 업데이트 (health check)
    track_request_end(&pool, server_idx, request_success, response_time);

    // 서버별 메트릭 로깅
    log_server_metrics(server->address, server->port,
                       server->current_requests,
                       server->total_requests,
                       server->total_failures,
                       server->avg_response_time);

    // 전체 시스템 메트릭 로깅
    log_system_metrics(pool.total_requests,
                       pool.total_failures,
                       pool.avg_response_time);

    close(client_socket);
    close(target_socket);
    free(conn_info);
    return NULL;
}

int run_proxy(int listen_port)
{
    int listen_socket;
    struct sockaddr_in listen_addr, client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    // 각 서버의 초기 상태 설정
    // 뮤텍스 초기화 (pool_mutex, server_mutex)
    init_backend_pool(&pool);
    log_message(LOG_INFO, "Backend server pool initialized with %d servers", MAX_BACKENDS);

    // 리스닝 소켓 설정
    listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket < 0)
    {
        log_message(LOG_ERROR, "Failed to create socket");
        return 1;
    }

    // SO_REUSEADDR 옵션 설정
    int reuse = 1;
    if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        log_message(LOG_ERROR, "Failed to set socket options");
        return 1;
    }

    // 리스닝 주소 설정
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(listen_port);
    listen_addr.sin_addr.s_addr = INADDR_ANY;

    // 소켓 바인딩
    if (bind(listen_socket, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0)
    {
        log_message(LOG_ERROR, "Failed to bind socket");
        close(listen_socket);
        return 1;
    }

    // 리스닝 시작
    if (listen(listen_socket, 10) < 0)
    {
        log_message(LOG_ERROR, "Failed to listen");
        close(listen_socket);
        return 1;
    }

    log_message(LOG_INFO, "Reverse proxy server listening on port %d", listen_port);

    while (1)
    {

        int client_socket = accept(listen_socket, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_socket < 0)
        {
            log_message(LOG_ERROR, "Failed to accept connection");
            continue;
        }

        struct connection_info *conn_info = malloc(sizeof(struct connection_info));
        conn_info->client_socket = client_socket;
        conn_info->client_addr = client_addr;

        // 새로운 스레드를 생성하여 클라이언트 처리
        pthread_t thread_id;
        if (pthread_create(&thread_id, NULL, handle_client, (void *)conn_info) != 0)
        {
            log_message(LOG_ERROR, "Failed to create thread");
            free(conn_info);
            close(client_socket);
            continue;
        }

        // 스레드를 detach 모드로 설정 (자동 리소스 정리)
        pthread_detach(thread_id);
    }

    // 프로그램 종료 시 리소스 정리
    cleanup_backend_pool(&pool);
    close(listen_socket);
    return 0;
}