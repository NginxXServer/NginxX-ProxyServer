#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include "health.h"
#include "../utils/logger.h"

#define BUFFER_SIZE 9999
#define MAX_EVENTS 100

static struct backend_pool pool;
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

// 새로운 클라이언트 연결 처리
static void handle_new_connection(int epoll_fd, int listen_fd)
{
    // 하나의 이벤트로 여러 연결 요청이 들어올 수 있으므로 iteration
    while (1)
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // 큐에 있는 연결 처리
        int client_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // 더 이상 처리할 연결이 없음
                break;
            }
            log_message(LOG_ERROR, "Accept failed: %s", strerror(errno));
            break;
        }

        // 비동기 설정
        if (set_nonblocking(client_fd) < 0)
        {
            log_message(LOG_ERROR, "Failed to set client socket non-blocking");
            close(client_fd);
            continue;
        }

        log_message(LOG_INFO, "New connection from %s", inet_ntoa(client_addr.sin_addr));

        // edge trigger 모드로 client 요청 처리
        struct epoll_event event;
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = client_fd;

        // 새로운 클라이언트를 epoll_fd에 추가
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &event) < 0)
        {
            log_message(LOG_ERROR, "Failed to add client to epoll: %s", strerror(errno));
            close(client_fd);
            continue;
        }
    }
}

static void handle_client(int client_fd)
{
    char buffer[BUFFER_SIZE];
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    ssize_t bytes_read;

    getpeername(client_fd, (struct sockaddr *)&client_addr, &addr_len);
    const char *client_ip = inet_ntoa(client_addr.sin_addr);

    ssize_t total_read = 0;
    while (1)
    {
        // 모든 데이터 한 번에 읽기
        bytes_read = recv(client_fd, buffer + total_read,
                          BUFFER_SIZE - total_read - 1, 0);
        if (bytes_read < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break; // 더 이상 읽을 데이터가 없음
            }
            log_message(LOG_ERROR, "Failed to receive from client %s: %s",
                        client_ip, strerror(errno));
            close(client_fd);
            return;
        }
        if (bytes_read == 0)
        { // 연결 종료
            close(client_fd);
            return;
        }

        total_read += bytes_read;

        // HTTP 요청의 끝을 발견하면 중단
        if (total_read >= BUFFER_SIZE - 1 || strstr(buffer, "\r\n\r\n"))
        {
            break;
        }
    }

    if (total_read == 0)
    {
        close(client_fd);
        return;
    }

    buffer[total_read] = '\0';
    log_message(LOG_INFO, "Received %zd bytes from client %s", total_read, client_ip);

    // 백엔드 서버 선택
    int server_idx = select_server();
    if (server_idx < 0)
    {
        log_message(LOG_ERROR, "Failed to select backend server for client %s", client_ip);
        close(client_fd);
        return;
    }

    struct backend_server *server = &pool.servers[server_idx];
    track_request_start(&pool, server_idx);

    // 백엔드 연결
    int backend_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (backend_fd < 0)
    {
        log_message(LOG_ERROR, "Failed to create backend socket: %s", strerror(errno));
        close(client_fd);
        track_request_end(&pool, server_idx, false, 0);
        return;
    }

    struct sockaddr_in backend_addr;
    memset(&backend_addr, 0, sizeof(backend_addr));
    backend_addr.sin_family = AF_INET;
    backend_addr.sin_port = htons(server->port);
    backend_addr.sin_addr.s_addr = inet_addr(server->address);

    if (connect(backend_fd, (struct sockaddr *)&backend_addr, sizeof(backend_addr)) < 0)
    {
        log_message(LOG_ERROR, "Failed to connect to backend %s:%d: %s",
                    server->address, server->port, strerror(errno));
        close(client_fd);
        close(backend_fd);
        track_request_end(&pool, server_idx, false, 0);
        return;
    }

    // 백엔드로 요청 전송
    ssize_t sent = send(backend_fd, buffer, total_read, 0); // 클라이언트로부터 받은 만큼 전송함
    if (sent < 0)
    {
        log_message(LOG_ERROR, "Failed to send to backend %s:%d: %s",
                    server->address, server->port, strerror(errno));
        goto cleanup;
    }
    log_message(LOG_INFO, "Sent %zd bytes to backend %s:%d",
                sent, server->address, server->port);

    // 백엔드로부터 응답 수신, 클라이언트로부터 요청 수신과 거의 똑같은 알고리즘
    total_read = 0;
    while (1)
    {
        bytes_read = recv(backend_fd, buffer + total_read,
                          BUFFER_SIZE - total_read - 1, 0);
        if (bytes_read < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            log_message(LOG_ERROR, "Failed to receive from backend %s:%d: %s",
                        server->address, server->port, strerror(errno));
            goto cleanup;
        }
        if (bytes_read == 0)
        { // 연결 종료
            break;
        }
        total_read += bytes_read;

        if (total_read >= BUFFER_SIZE - 1)
        {
            break;
        }
    }

    if (total_read == 0)
    {
        goto cleanup;
    }

    buffer[total_read] = '\0';
    log_message(LOG_INFO, "Received %zd bytes from backend %s:%d",
                total_read, server->address, server->port);

    // 클라이언트로 응답 전송, total_read로 받은 응답 만큼 전송
    sent = send(client_fd, buffer, total_read, 0);
    if (sent < 0)
    {
        log_message(LOG_ERROR, "Failed to send response to client %s: %s",
                    client_ip, strerror(errno));
    }
    else
    {
        log_message(LOG_INFO, "Sent %zd bytes to client %s", sent, client_ip);
    }

cleanup:
    close(backend_fd);
    close(client_fd);
    track_request_end(&pool, server_idx, bytes_read > 0, 0);
}

int run_proxy(int listen_port)
{
    // 백엔드 풀 초기화
    init_backend_pool(&pool);
    log_message(LOG_INFO, "Backend server pool initialized with %d servers", MAX_BACKENDS);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
        return 1;

    // 소켓 재사용 설정
    int reuse = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 비동기로 non blocking socekt 사용
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

    // epoll 인스턴스 생성
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0)
    {
        close(listen_fd);
        return 1;
    }

    struct epoll_event event;
    event.events = EPOLLIN | EPOLLET; // Edge trigger 모드
    event.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &event); // epoll_fd가 listen_fd를 event 방식으로 감시함.

    log_message(LOG_INFO, "Reverse proxy server listening on port %d", listen_port);

    struct epoll_event events[MAX_EVENTS]; // 발생한 이벤트들 저장
    while (1)
    {
        // 이벤트 발생 대기, 발생한 이벤트는 events에 추가됨.
        // 한 iteration을 돌면 events는 덮어 씌워짐
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0)
            continue;

        // 발생한 event에 대해 처리
        for (int n = 0; n < nfds; n++)
        {
            if (events[n].data.fd == listen_fd)
            {
                // handle_new_connection를 통해 client 요청을 epoll 감시 대상으로 등록
                handle_new_connection(epoll_fd, listen_fd);
            }
            else
            {
                // client 요청 처리 및 클라이언트 연결 종료
                handle_client(events[n].data.fd);
            }
        }
    }

    close(epoll_fd);
    close(listen_fd);
    return 0;
}