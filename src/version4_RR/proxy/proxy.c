#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
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
static int current_server = 0;

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
    // 서버 구성 확인
    if (MAX_BACKENDS <= 0)
    {
        log_message(LOG_ERROR, "No backend servers configured");
        return -1;
    }

    // 라운드 로빈 방식으로 서버 선택
    int selected = current_server;
    current_server = (current_server + 1) % MAX_BACKENDS;

    // 선택된 서버의 유효성 확인
    struct backend_server *server = &pool.servers[selected];
    if (server->address != NULL && server->port > 0)
    {
        log_message(LOG_INFO, "Selected backend server %s:%d",
                    server->address, server->port);
        return selected;
    }
    else
    {
        log_message(LOG_ERROR, "Invalid server configuration at index %d", selected);
        return -1;
    }
}

void handle_connection(int client_fd, struct sockaddr_in client_addr)
{
    char buffer[BUFFER_SIZE];
    ssize_t bytes_received;

    // 클라이언트 요청 읽기(non blcoking)
    while ((bytes_received = recv(client_fd, buffer, BUFFER_SIZE - 1, 0)) > 0)
    {
        buffer[bytes_received] = '\0';

        // 백엔드 서버 선택
        int server_idx = select_server();
        if (server_idx < 0)
        {
            close(client_fd);
            return;
        }

        int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (backend_fd < 0)
        {
            close(client_fd);
            return;
        }

        struct backend_server *server = &backend_pool.servers[server_idx];
        struct sockaddr_in backend_addr;
        memset(&backend_addr, 0, sizeof(backend_addr));
        backend_addr.sin_family = AF_INET;
        backend_addr.sin_port = htons(server->port);
        backend_addr.sin_addr.s_addr = inet_addr(server->address);

        // 백엔드 연결 (blocking)
        if (connect(backend_fd, (struct sockaddr *)&backend_addr, sizeof(backend_addr)) < 0)
        {
            close(backend_fd);
            close(client_fd);
            return;
        }

        // 요청을 백엔드로 전달
        if (send(backend_fd, buffer, bytes_received, 0) < 0)
        {
            close(backend_fd);
            close(client_fd);
            return;
        }

        // 백엔드 응답 받기 (blocking)
        char response[BUFFER_SIZE];
        ssize_t response_size = recv(backend_fd, response, BUFFER_SIZE - 1, 0);
        if (response_size > 0)
        {
            response[response_size] = '\0';

            // 응답을 클라이언트에게 전송 (non-blocking)
            send(client_fd, response, response_size, MSG_DONTWAIT);
        }

        close(backend_fd);
    }

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

    // 클라이언트 소켓은 non blocking!
    if (set_nonblocking(client_fd) < 0)
    {
        close(client_fd);
        return;
    }

    // 작업을 스레드 풀에 추가
    if (thread_pool_add_work(&thread_pool, client_fd, client_addr) < 0)
    {
        close(client_fd);
        return;
    }

    log_message(LOG_INFO, "New connection from %s added to work queue",
                inet_ntoa(client_addr.sin_addr));
}

int run_proxy(int listen_port)
{
    // 백엔드 서버 초기화
    init_backend_pool(&pool);
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
        /*
         * EPOLLIN : 데이터가 도착해서 읽을 수 있음
         * EPOLLOUT : 데이터를 보낼 수 있음
         * 위의 두 event를 사용하여 이벤트 구분 및 처리
         */

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
                // 새로운 클라이언트 연결 처리
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);

                int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
                if (client_fd < 0)
                {
                    continue;
                }

                // 작업을 스레드 풀에 추가
                if (thread_pool_add_work(&thread_pool, client_fd, client_addr) < 0)
                {
                    close(client_fd);
                    continue;
                }

                log_message(LOG_INFO, "New connection from %s added to work queue",
                            inet_ntoa(client_addr.sin_addr));
            }
        }
    }

    // 리소스 정리
    thread_pool_destroy(&thread_pool);
    close(epoll_fd);
    close(listen_fd);
    return 0;
}