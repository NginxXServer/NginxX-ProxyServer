#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <time.h>
#include "health.h"
#include "../utils/logger.h"

#define BUFFER_SIZE 9999
#define MAX_EVENTS 10000

static struct backend_pool pool;
static int current_server = 0;

struct connection_state {
    int fd;                     // 소켓 파일 디스크립터
    bool is_backend;            // backend 연결인지 여부
    int backend_fd;             
    int client_fd;              
    char buffer[BUFFER_SIZE];   
    size_t buffer_size;        
    struct sockaddr_in addr;    
    struct timespec start_time; 
};

// non-blocking 소켓 설정 함수
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        log_message(LOG_ERROR, "Failed to get socket flags");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        log_message(LOG_ERROR, "Failed to set socket non-blocking");
        return -1;
    }
    return 0;
}

/**
 * 백엔드 서버 선택 함수 (라운드 로빈 방식) -> 이 부분 수정 필요
 * - 서버 선택 시 health check 결과를 반영
 * 
 * 반환값:
 * - 성공: 선택된 서버의 인덱스
 * - 실패: -1 (사용 가능한 서버가 없는 경우)
 */
int select_server() {
    int selected = current_server;
    current_server = (current_server + 1) % MAX_BACKENDS;
    
    log_message(LOG_INFO, "Selected backend server %s:%d", 
        pool.servers[selected].address, 
        pool.servers[selected].port);
    
    return selected;
}

// epoll에 이벤트 추가
static void add_epoll_event(int epoll_fd, int fd, struct connection_state* state, uint32_t events) {
    struct epoll_event ev;
    ev.events = events;
    ev.data.ptr = state;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        log_message(LOG_ERROR, "Failed to add event to epoll");
    }
}

// 새로운 클라이언트 연결 처리
static void handle_new_connection(int epoll_fd, int listen_fd) {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    
    int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_addr_len);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            log_message(LOG_ERROR, "Failed to accept connection");
        }
        return;
    }
    
    if (set_nonblocking(client_fd) < 0) {
        close(client_fd);
        return;
    }
    
    struct connection_state* state = calloc(1, sizeof(struct connection_state));
    state->fd = client_fd;
    state->is_backend = false;
    state->addr = client_addr;
    clock_gettime(CLOCK_MONOTONIC, &state->start_time);
    
    add_epoll_event(epoll_fd, client_fd, state, EPOLLIN | EPOLLET);
    log_message(LOG_INFO, "New connection from %s", inet_ntoa(client_addr.sin_addr));
}

// 백엔드 서버로 연결
static void connect_to_backend(int epoll_fd, struct connection_state* client_state) {
    int server_idx = select_server();
    if (server_idx < 0) {
        free(client_state);
        close(client_state->fd);
        return;
    }
    
    struct backend_server* server = &pool.servers[server_idx];
    int backend_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (backend_fd < 0) {
        log_message(LOG_ERROR, "Failed to create backend socket");
        free(client_state);
        close(client_state->fd);
        return;
    }
    
    struct sockaddr_in backend_addr;
    memset(&backend_addr, 0, sizeof(backend_addr));
    backend_addr.sin_family = AF_INET;
    backend_addr.sin_port = htons(server->port);
    backend_addr.sin_addr.s_addr = inet_addr(server->address);
    
    track_request_start(&pool, server_idx);
    
    if (connect(backend_fd, (struct sockaddr*)&backend_addr, sizeof(backend_addr)) < 0) {
        if (errno != EINPROGRESS) {
            log_message(LOG_ERROR, "Failed to connect to backend");
            track_request_end(&pool, server_idx, false, 0);
            free(client_state);
            close(client_state->fd);
            close(backend_fd);
            return;
        }
    }
    
    struct connection_state* backend_state = calloc(1, sizeof(struct connection_state));
    backend_state->fd = backend_fd;
    backend_state->is_backend = true;
    backend_state->client_fd = client_state->fd;
    client_state->backend_fd = backend_fd;
    
    add_epoll_event(epoll_fd, backend_fd, backend_state, EPOLLIN | EPOLLOUT | EPOLLET);
}

// 데이터 전송 처리
static void handle_data(int epoll_fd, struct connection_state* state, uint32_t events) {
    if (events & EPOLLIN) {
        // 데이터 읽기
        ssize_t bytes_received = recv(state->fd, 
                                    state->buffer + state->buffer_size,
                                    BUFFER_SIZE - state->buffer_size, 0);
                                    
        if (bytes_received <= 0) {
            if (bytes_received == 0 || errno != EAGAIN) {
                // 연결 종료 또는 에러
                close(state->fd);
                if (!state->is_backend) {
                    close(state->backend_fd);
                } else {
                    close(state->client_fd);
                }
                free(state);
                return;
            }
        } else {
            state->buffer_size += bytes_received;
            state->buffer[state->buffer_size] = '\0';
            
            int target_fd = state->is_backend ? state->client_fd : state->backend_fd;
            ssize_t bytes_sent = send(target_fd, state->buffer, state->buffer_size, 0);
            
            if (bytes_sent < 0 && errno != EAGAIN) {
                // 전송 에러 처리
                close(state->fd);
                close(target_fd);
                free(state);
                return;
            }
            
            // 버퍼 리셋
            state->buffer_size = 0;
        }
    }
}

int run_proxy(int listen_port) {
    int listen_fd;
    struct sockaddr_in listen_addr;
    
    // 백엔드 풀 초기화
    init_backend_pool(&pool);
    log_message(LOG_INFO, "Backend server pool initialized with %d servers", MAX_BACKENDS);
    
    // 리스닝 소켓 설정
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        log_message(LOG_ERROR, "Failed to create socket");
        return 1;
    }
    
    // SO_REUSEADDR 옵션 설정
    int reuse = 1;
    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        log_message(LOG_ERROR, "Failed to set socket options");
        return 1;
    }
    
    // non-blocking 설정
    if (set_nonblocking(listen_fd) < 0) {
        return 1;
    }
    
    // 리스닝 주소 설정
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(listen_port);
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    
    // 소켓 바인딩
    if (bind(listen_fd, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
        log_message(LOG_ERROR, "Failed to bind socket");
        close(listen_fd);
        return 1;
    }
    
    // 리스닝 시작
    if (listen(listen_fd, SOMAXCONN) < 0) {
        log_message(LOG_ERROR, "Failed to listen");
        close(listen_fd);
        return 1;
    }
    
    // epoll 인스턴스 생성
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        log_message(LOG_ERROR, "Failed to create epoll instance");
        close(listen_fd);
        return 1;
    }
    
    // listen 소켓을 epoll에 추가
    struct connection_state* listen_state = calloc(1, sizeof(struct connection_state));
    listen_state->fd = listen_fd;
    add_epoll_event(epoll_fd, listen_fd, listen_state, EPOLLIN | EPOLLET);
    
    log_message(LOG_INFO, "Reverse proxy server listening on port %d", listen_port);
    
    // 이벤트 루프
    struct epoll_event events[MAX_EVENTS];
    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            if (errno != EINTR) {
                log_message(LOG_ERROR, "epoll_wait failed");
                break;
            }
            continue;
        }
        
        for (int n = 0; n < nfds; n++) {
            struct connection_state* state = events[n].data.ptr;
            
            if (state->fd == listen_fd) {
                // 새로운 연결 처리
                handle_new_connection(epoll_fd, listen_fd);
            } else if (!state->is_backend && state->backend_fd == -1) {
                // 새로운 클라이언트 요청을 백엔드로 연결
                connect_to_backend(epoll_fd, state);
            } else {
                // 데이터 전송 처리
                handle_data(epoll_fd, state, events[n].events);
            }
        }
    }
    
    // 리소스 정리
    close(epoll_fd);
    close(listen_fd);
    free(listen_state);
    
    return 0;
}