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
#include <limits.h>
#include "health.h"
#include "../utils/logger.h"

#define BUFFER_SIZE 9999
#define MAX_EVENTS 100

// 클라이언트와 HTTP 서버 간의 연결 상태를 추적하기 위한 구조체
// 각 연결마다 하나의 인스턴스 사용
struct connection
{
    int client_fd;
    int backend_fd;
    char buffer[BUFFER_SIZE];
    size_t bytes_received;
    size_t bytes_sent;
    int server_idx;
    int is_backend_connected;
    struct sockaddr_in client_addr;
};

static struct backend_pool pool;

// connection 초기화
static struct connection *create_connection(int client_fd, struct sockaddr_in client_addr)
{
    struct connection *conn = (struct connection *)malloc(sizeof(struct connection));
    if (!conn)
        return NULL;

    conn->client_fd = client_fd;
    conn->backend_fd = -1;
    conn->bytes_received = 0;
    conn->bytes_sent = 0;
    conn->server_idx = -1;
    conn->is_backend_connected = 0;
    conn->client_addr = client_addr;
    memset(conn->buffer, 0, BUFFER_SIZE);

    return conn;
}

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
    for (int i = 0; i < pool.server_count; i++)
    {
        struct backend_server *server = &pool.servers[i];

        // 서버가 유효하고 헬스 상태가 "정상"일 때만 고려
        if (server->is_healthy && server->current_requests < min_connections)
        {
            min_connections = server->current_requests;
            selected = i;
        }
    }
    if (selected == -1)
    {
        log_message(LOG_ERROR, "No healthy backend servers available");
    }
    else
    {
        struct backend_server *selected_server = &pool.servers[selected];
    }

    return selected;
}

// 연결 종료시 호출
static void cleanup_connection(struct connection *conn)
{
    if (conn->server_idx >= 0)
    {
        track_request_end(&pool, conn->server_idx, 1, 0);
    }
    if (conn->client_fd >= 0)
        close(conn->client_fd);
    if (conn->backend_fd >= 0)
        close(conn->backend_fd);
    free(conn);
}

// 클라이언트의 데이터를 읽기
static void handle_client_read(int epoll_fd, struct connection *conn)
{
    ssize_t bytes_read = recv(conn->client_fd,
                              conn->buffer + conn->bytes_received,
                              BUFFER_SIZE - conn->bytes_received - 1,
                              0);

    if (bytes_read <= 0)
    {
        if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return; // 다 읽음
        }
        cleanup_connection(conn);
        return;
    }

    conn->bytes_received += bytes_read;
    conn->buffer[conn->bytes_received] = '\0';

    // HTTP 요청이 완전히 수신되었는지 확인
    if (!conn->is_backend_connected && strstr(conn->buffer, "\r\n\r\n"))
    {
        // 백엔드 서버 선택
        conn->server_idx = select_server();
        if (conn->server_idx < 0)
        {
            cleanup_connection(conn);
            return;
        }

        struct backend_server *server = &pool.servers[conn->server_idx];
        track_request_start(&pool, conn->server_idx);

        // 백엔드 연결 설정
        conn->backend_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (conn->backend_fd < 0)
        {
            cleanup_connection(conn);
            return;
        }

        struct sockaddr_in backend_addr;
        memset(&backend_addr, 0, sizeof(backend_addr));
        backend_addr.sin_family = AF_INET;
        backend_addr.sin_port = htons(server->port);
        backend_addr.sin_addr.s_addr = inet_addr(server->address);

        // 백엔드와 연결
        if (connect(conn->backend_fd, (struct sockaddr *)&backend_addr, sizeof(backend_addr)) < 0)
        {
            if (errno != EINPROGRESS)
            {
                cleanup_connection(conn);
                return;
            }
        }

        // 백엔드로 요청을 보내면 됨
        struct epoll_event ev;
        ev.events = EPOLLOUT;
        ev.data.ptr = conn;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn->backend_fd, &ev) < 0)
        {
            log_message(LOG_ERROR, "Failed to add backend socket to epoll: %s", strerror(errno));
            cleanup_connection(conn);
            return;
        }

        log_message(LOG_INFO, "Backend connection initiated to %s:%d",
                    server->address, server->port);
        conn->is_backend_connected = 1;
    }
}

static void handle_backend_connect(int epoll_fd, struct connection *conn)
{
    int error;
    socklen_t len = sizeof(error);

    // 연결 상태 확인
    if (getsockopt(conn->backend_fd, SOL_SOCKET, SO_ERROR, &error, &len) < 0)
    {
        log_message(LOG_ERROR, "Failed to get socket error status: %s", strerror(errno));
        cleanup_connection(conn);
        return;
    }

    if (error != 0)
    {
        log_message(LOG_ERROR, "Backend connection failed: %s", strerror(error));
        cleanup_connection(conn);
        return;
    }

    log_message(LOG_INFO, "Backend connection established successfully");

    struct epoll_event ev;
    ev.events = EPOLLIN; // 우선 이번 연결에서 모두 처리했다고 가정하고 EPOLLIN만 남겨둠
    ev.data.ptr = conn;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->backend_fd, &ev) < 0)
    {
        log_message(LOG_ERROR, "Failed to modify backend socket events: %s", strerror(errno));
        cleanup_connection(conn);
        return;
    }

    // 클라이언트로부터 받은 데이터를 백엔드로 전송
    ssize_t sent = send(conn->backend_fd, conn->buffer, conn->bytes_received, 0);
    if (sent < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // 다음 iteration에서 다시 시도하도록 EPOLLOUT 이벤트 추가
            ev.events = EPOLLIN | EPOLLOUT;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->backend_fd, &ev) < 0)
            {
                cleanup_connection(conn);
                return;
            }
            return;
        }
        log_message(LOG_ERROR, "Failed to send data to backend: %s", strerror(errno));
        cleanup_connection(conn);
        return;
    }

    conn->bytes_sent = sent;
}

static void handle_backend_read(int epoll_fd, struct connection *conn)
{
    char response[BUFFER_SIZE];
    ssize_t bytes_read = recv(conn->backend_fd, response, BUFFER_SIZE - 1, 0);

    if (bytes_read <= 0)
    {
        if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return;
        }
        cleanup_connection(conn);
        return;
    }

    response[bytes_read] = '\0';

    // 클라이언트에게 응답 전송
    ssize_t sent = send(conn->client_fd, response, bytes_read, 0);
    if (sent < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            cleanup_connection(conn);
            return;
        }
    }
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

    // 비동기 설정
    if (set_nonblocking(client_fd) < 0)
    {
        close(client_fd);
        return;
    }

    // connection 구조체 생성
    struct connection *conn = create_connection(client_fd, client_addr);
    if (!conn)
    {
        close(client_fd);
        return;
    }

    // Level Trigger 모드로 이벤트 설정
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.ptr = conn;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0)
    {
        free(conn);
        close(client_fd);
        return;
    }

    log_message(LOG_INFO, "New connection from %s", inet_ntoa(client_addr.sin_addr));
}

int run_proxy(int listen_port)
{
    // 백엔드 서버 초기화
    init_backend_pool(&pool);
    log_message(LOG_INFO, "Backend server pool initialized with %d servers", MAX_BACKENDS);

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
                // 새로운 연결 요청인 경우
                handle_new_connection(epoll_fd, listen_fd);
            }
            else
            {
                // 기존에 연결되어있던 클라이언트의 경우 정보 가져오기
                struct connection *conn = (struct connection *)events[n].data.ptr;
                if (!conn)
                    continue;

                if (events[n].events & (EPOLLERR | EPOLLHUP))
                {
                    log_message(LOG_ERROR, "Socket error or hangup detected");
                    cleanup_connection(conn);
                    continue;
                }

                if (events[n].events & EPOLLOUT)
                {
                    // 아직 리버스 프록시가 정보를 보내고 있는 상태인 경우
                    if (!conn->is_backend_connected || conn->bytes_sent < conn->bytes_received)
                    {
                        handle_backend_connect(epoll_fd, conn);
                        continue;
                    }
                }

                if (events[n].events & EPOLLIN)
                {
                    // 백엔드 혹은 클라이언트의 데이터를 읽어야하는 경우
                    if (conn->backend_fd == -1)
                    {
                        // client에서 읽기
                        handle_client_read(epoll_fd, conn);
                    }
                    else
                    {
                        handle_backend_read(epoll_fd, conn);
                    }
                }
            }
        }
    }

    close(epoll_fd);
    close(listen_fd);
    return 0;
}