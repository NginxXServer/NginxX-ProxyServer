#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include "proxy.h"
#include "threadpool.h"
#include "health.h"

#include "../utils/logger.h"

#define BUFFER_SIZE 9999
#define MAX_EVENTS 100
#define NUM_THREADS 4

static struct backend_pool backend_pool;
static struct thread_pool thread_pool;
static pthread_mutex_t server_select_mutex = PTHREAD_MUTEX_INITIALIZER;
static int current_server = 0;
static atomic_uint request_counter = 0;
static atomic_int current_server_atomic = 0;

// non-blocking 소켓 설정
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * HTTP 서버 선택 함수 (라운드 로빈 방식, 뮤텍스 없음)
 *
 * 반환값:
 * - 성공: 선택된 서버의 인덱스
 * - 실패: -1
 */
int select_server()
{
    // // 라운드 로빈 방식으로 서버 선택
    // pthread_mutex_lock(&server_select_mutex);

    // 서버 구성 확인
    if (MAX_BACKENDS <= 0)
    {
        pthread_mutex_unlock(&server_select_mutex);
        log_message(LOG_ERROR, "No backend servers configured");
        return -1;
    }

    // int selected = current_server;
    // current_server = (current_server + 1) % MAX_BACKENDS;
    int selected = atomic_fetch_add(&current_server_atomic, 1) % MAX_BACKENDS;

    // 선택된 서버의 유효성 확인
    struct backend_server *server = &backend_pool.servers[selected];
    if (server->address != NULL && server->port > 0)
    {
        log_message(LOG_INFO, "Selected backend server %s:%d",
                    server->address, server->port);
        // pthread_mutex_unlock(&server_select_mutex);
        return selected;
    }
    // pthread_mutex_unlock(&server_select_mutex);
    log_message(LOG_ERROR, "Invalid server configuration at index %d", selected);
    return -1;
}

void handle_connection(int client_fd, struct sockaddr_in client_addr)
{
    unsigned int req_num = atomic_fetch_add(&request_counter, 1);
    char request_id[32];
    snprintf(request_id, sizeof(request_id), "REQ-%d-%u", client_fd, req_num);

    log_message(LOG_INFO, "[%s] New request started from IP: %s",
                request_id, inet_ntoa(client_addr.sin_addr));

    // 클라이언트로부터 요청 받기
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);

    if (bytes_received <= 0)
    {
        close(client_fd);
        return;
    }

    buffer[bytes_received] = '\0';

    // 백엔드 서버 선택 및 연결
    int server_idx = select_server();
    if (server_idx < 0)
    {
        close(client_fd);
        return;
    }

    // 요청 시작 기록
    track_request_start(&backend_pool, server_idx);
    struct backend_server *server = &backend_pool.servers[server_idx];

    log_message(LOG_INFO, "[%s] Selected backend server %s:%d",
                request_id, server->address, server->port);

    int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (backend_fd < 0)
    {
        track_request_end(&backend_pool, server_idx, 0, 1); // 실패 기록
        close(client_fd);
        return;
    }

    struct sockaddr_in backend_addr;
    memset(&backend_addr, 0, sizeof(backend_addr));
    backend_addr.sin_family = AF_INET;
    backend_addr.sin_port = htons(server->port);
    backend_addr.sin_addr.s_addr = inet_addr(server->address);

    if (connect(backend_fd, (struct sockaddr *)&backend_addr, sizeof(backend_addr)) < 0)
    {
        track_request_end(&backend_pool, server_idx, 0, 1); // 실패 기록
        close(backend_fd);
        close(client_fd);
        return;
    }

    // 백엔드로 요청 전송
    if (send(backend_fd, buffer, bytes_received, 0) < 0)
    {
        track_request_end(&backend_pool, server_idx, 0, 1); // 실패 기록
        close(backend_fd);
        close(client_fd);
        return;
    }

    // 백엔드로부터 응답 받기
    char response[BUFFER_SIZE];
    size_t total_received = 0;
    int success = 0;

    // 응답을 완전히 받을 때까지 반복
    while (1)
    {
        bytes_received = recv(backend_fd, response + total_received,
                              BUFFER_SIZE - total_received - 1, 0);

        if (bytes_received < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        else if (bytes_received == 0) // 연결 종료
            break;

        total_received += bytes_received;

        // 버퍼가 가득 차면 중단
        if (total_received >= BUFFER_SIZE - 1)
            break;
    }

    if (total_received > 0)
    {
        response[total_received] = '\0';

        // 클라이언트로 전체 응답 전송
        size_t total_sent = 0;
        while (total_sent < total_received)
        {
            ssize_t sent = send(client_fd, response + total_sent,
                                total_received - total_sent, 0);
            if (sent < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    continue;
                break;
            }
            total_sent += sent;
        }
        success = (total_sent == total_received); // 모든 데이터가 전송되었는지 확인
    }

    if (success)
    {
        log_message(LOG_INFO, "[%s] Request completed successfully - Sent %zu bytes",
                    request_id, total_received);
    }
    else
    {
        log_message(LOG_ERROR, "[%s] Request failed during processing", request_id);
    }

    // 요청 추적 끝
    track_request_end(&backend_pool, server_idx, success, !success);

    close(backend_fd);
    close(client_fd);
}

static void handle_new_connection(int epoll_fd, int listen_fd)
{
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0)
    {
        return;
    }

    // client_fd를 non-blocking으로 설정하기 전에 먼저 스레드풀에 작업 추가
    if (thread_pool_add_work(&thread_pool, client_fd, client_addr) < 0)
    {
        close(client_fd);
        return;
    }
}

int run_proxy(int listen_port)
{
    // 백엔드 서버 초기화
    init_backend_pool(&backend_pool);

    log_message(LOG_INFO, "Backend server pool initialized with %d servers", MAX_BACKENDS);

    // 스레드 풀 초기화
    if (thread_pool_init(&thread_pool, NUM_THREADS) < 0)
    {
        log_message(LOG_ERROR, "Failed to initialize thread pool");
        return 1;
    }
    log_message(LOG_INFO, "Thread pool initialized with %d threads", NUM_THREADS);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
        return 1;

    // 포트 번호 재사용 설정
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // listen_fd 비동기로 설정
    set_nonblocking(listen_fd);

    struct sockaddr_in listen_addr;
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(listen_port);
    listen_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0)
    {
        close(listen_fd);
        return 1;
    }

    if (listen(listen_fd, SOMAXCONN) < 0)
    {
        close(listen_fd);
        return 1;
    }

    // epoll 생성
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
    {
        close(listen_fd);
        return 1;
    }

    // listen_fd를 epoll event에 추가 및 epoll event 설정
    struct epoll_event ev;
    ev.events = EPOLLIN; // Level Trigger 모드 사용
    ev.data.fd = listen_fd;

    // epoll 설정
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) < 0)
    {
        close(epoll_fd);
        close(listen_fd);
        return 1;
    }

    struct epoll_event events[MAX_EVENTS];
    while (1)
    {

        // 이벤트가 발생하기를 대기, 이벤트가 발생하면 events 배열에 저장하고 nfds에 이벤트 개수 저장
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        for (int n = 0; n < nfds; n++)
        {
            if (events[n].data.fd == listen_fd)
            {
                handle_new_connection(epoll_fd, listen_fd);
            }
        }
    }

    // 리소스 정리
    thread_pool_destroy(&thread_pool);
    close(epoll_fd);
    close(listen_fd);
    return 0;
}