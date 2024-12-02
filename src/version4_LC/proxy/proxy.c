#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>
#include <stdatomic.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include "proxy.h"
#include "threadpool.h"
#include "health.h"

#include "../utils/logger.h"

#define BUFFER_SIZE 9999
#define MAX_EVENTS 100
#define NUM_THREADS 6
#define CHUNK_SIZE (1024 * 1024)

static struct backend_pool backend_pool;
static struct thread_pool thread_pool;
static atomic_uint request_counter = 0;

// non-blocking 소켓 설정
static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * HTTP 서버 선택 함수 (Least-Connection 방식)
 *
 * 반환값:
 * - 성공: 선택된 서버의 인덱스
 * - 실패: -1
 */
int select_server()
{
    int selected = -1;
    int min_connections = INT_MAX;

    // 모든 서버를 순회하며 가장 적은 요청 수를 가진 서버를 찾음
    for (int i = 0; i < backend_pool.server_count; i++)
    {
        struct backend_server *server = &backend_pool.servers[i];

        if (!server->is_healthy && server->current_requests == 0)
        {
            server->is_healthy = true;
            server->failed_responses = 0; // 실패 카운트 리셋
        }

        // 서버가 유효하고 헬스 상태가 "정상"일 때만 고려
        if (server->is_healthy && server->current_requests < min_connections)
        {
            min_connections = server->current_requests;
            selected = i;
        }
    }
    if (selected == -1)
    {
        selected = 0;
        backend_pool.servers[0].is_healthy = true;
        backend_pool.servers[0].failed_responses = 0;
        log_message(LOG_INFO, "Forcing server 0 back to healthy state");
    }

    return selected;
}

void handle_connection(int client_fd, struct sockaddr_in client_addr)
{
    unsigned int req_num = atomic_fetch_add(&request_counter, 1);
    char request_id[32];
    snprintf(request_id, sizeof(request_id), "REQ-%d-%u", client_fd, req_num);

    // 클라이언트로부터 요청 받기
    char buffer[CHUNK_SIZE];
    ssize_t bytes_received;

    bytes_received = recv(client_fd, buffer, CHUNK_SIZE - 1, 0);
    if (bytes_received <= 0)
    {
        log_message(LOG_INFO, "[%s] Client connection closed or error", request_id);
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

    track_request_start(&backend_pool, server_idx);
    struct backend_server *server = &backend_pool.servers[server_idx];

    log_message(LOG_INFO, "[%s] Selected backend server %s:%d",
                request_id, server->address, server->port);

    int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (backend_fd < 0)
    {
        close(client_fd);
        track_request_end(&backend_pool, server_idx, 0, 1);
        return;
    }

    // 소켓 버퍼 크기 늘리기
    int buffer_size = 10485760; // 10MB
    setsockopt(backend_fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
    setsockopt(backend_fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));
    setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size));

    struct sockaddr_in backend_addr;
    memset(&backend_addr, 0, sizeof(backend_addr));
    backend_addr.sin_family = AF_INET;
    backend_addr.sin_port = htons(server->port);
    backend_addr.sin_addr.s_addr = inet_addr(server->address);

    if (connect(backend_fd, (struct sockaddr *)&backend_addr, sizeof(backend_addr)) < 0)
    {
        close(backend_fd);
        close(client_fd);
        track_request_end(&backend_pool, server_idx, 0, 1);
        return;
    }

    if (send(backend_fd, buffer, bytes_received, 0) < 0)
    {
        close(backend_fd);
        close(client_fd);
        track_request_end(&backend_pool, server_idx, 0, 1);
        return;
    }

    // 백엔드로부터 응답 받기
    char response[CHUNK_SIZE];
    bool success = true;

    while (1)
    {
        bytes_received = recv(backend_fd, response, CHUNK_SIZE, 0);
        if (bytes_received <= 0)
        {
            success = (bytes_received == 0); // 정상 종료인 경우는 성공으로 처리
            goto cleanup;
        }

        // 클라이언트로 청크 단위 전송
        size_t total_sent = 0;
        while (total_sent < bytes_received)
        {
            ssize_t sent = send(client_fd, response + total_sent,
                                bytes_received - total_sent, 0);
            if (sent < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    usleep(1000);
                    continue;
                }
                break;
            }
            total_sent += sent;
        }
    }

cleanup:
    shutdown(backend_fd, SHUT_RDWR);
    shutdown(client_fd, SHUT_RDWR);
    close(backend_fd);
    close(client_fd);
    track_request_end(&backend_pool, server_idx, success, !success);
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

    signal(SIGPIPE, SIG_IGN);

    while (1)
    {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000); // timeout 1초 설정

        if (nfds < 0)
        {
            if (errno == EINTR)
                continue;
            log_message(LOG_ERROR, "epoll_wait error: %s", strerror(errno));
            usleep(1000); // 에러시 잠시 대기
            continue;     // 에러가 나도 계속 실행
        }

        // 타임아웃인 경우 (nfds == 0)
        if (nfds == 0)
        {
            continue;
        }

        for (int n = 0; n < nfds; n++)
        {
            if (events[n].data.fd == listen_fd)
            {
                // 에러 처리 강화
                if (events[n].events & (EPOLLERR | EPOLLHUP))
                {
                    log_message(LOG_ERROR, "Listen socket error, attempting to recover...");
                    continue;
                }
                handle_new_connection(epoll_fd, listen_fd);
            }
        }
    }
    log_message(LOG_INFO, "Destroying Thread Pool...");
    thread_pool_destroy(&thread_pool);
    close(epoll_fd);
    close(listen_fd);
    return 0;
}