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
#include "health.h"
#include "../utils/logger.h"

#define BUFFER_SIZE 9999
static struct backend_pool pool;

// 현재 사용할 서버 선택 (간단한 라운드로빈)
static int current_server = 0;
int select_server() {
   int start_idx = current_server;
   
   do {
       if (is_server_available(&pool, current_server)) {
           int selected = current_server;
           current_server = (current_server + 1) % MAX_BACKENDS;
           log_message(LOG_INFO, "Selected backend server %s:%d", 
               pool.servers[selected].address, 
               pool.servers[selected].port);
           return selected;
       }
       current_server = (current_server + 1) % MAX_BACKENDS;
   } while (current_server != start_idx);
   
   log_message(LOG_ERROR, "No healthy backend servers available");
   return -1;  // 사용 가능한 서버가 없음
}

int run_proxy(int listen_port) {
   int listen_socket;
   struct sockaddr_in listen_addr, client_addr;  
   socklen_t client_addr_len = sizeof(client_addr);  
   char buffer[BUFFER_SIZE];
   int bytes_received;

   // 백엔드 서버 풀 초기화
   init_backend_pool(&pool);
   log_message(LOG_INFO, "Backend server pool initialized with %d servers", MAX_BACKENDS);

   // 리스닝 소켓 생성
   listen_socket = socket(AF_INET, SOCK_STREAM, 0);
   if (listen_socket < 0) {
       log_message(LOG_ERROR, "Failed to create socket");
       return 1;
   }

   // SO_REUSEADDR 옵션 설정
   int reuse = 1;
   if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
       log_message(LOG_ERROR, "Failed to set socket options");
       return 1;
   }

   // 리스닝 소켓 주소 설정
   memset(&listen_addr, 0, sizeof(listen_addr));
   listen_addr.sin_family = AF_INET;
   listen_addr.sin_port = htons(listen_port);
   listen_addr.sin_addr.s_addr = INADDR_ANY;

   // 리스닝 소켓 바인딩
   if (bind(listen_socket, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
       log_message(LOG_ERROR, "Failed to bind socket");
       close(listen_socket);
       return 1;
   }

   // 리스닝 모드 시작
   if (listen(listen_socket, 10) < 0) {
       log_message(LOG_ERROR, "Failed to listen");
       close(listen_socket);
       return 1;
   }

   log_message(LOG_INFO, "Reverse proxy server listening on port %d", listen_port);

   while (1) {
       int client_socket = accept(listen_socket, (struct sockaddr*)&client_addr, &client_addr_len);
       if (client_socket < 0) {
           log_message(LOG_ERROR, "Failed to accept connection");
           continue;
       }

       char* client_ip = inet_ntoa(client_addr.sin_addr);
       log_message(LOG_INFO, "New connection from %s", client_ip);

       // 백엔드 서버 선택
       int server_idx = select_server();
       if (server_idx < 0) {
           close(client_socket);
           continue;
       }

       struct backend_server* server = &pool.servers[server_idx];
       int target_socket = socket(AF_INET, SOCK_STREAM, 0);
       if (target_socket < 0) {
           log_message(LOG_ERROR, "Failed to create socket for backend connection");
           close(client_socket);
           continue;
       }

       // 요청 시작 시간 기록
       struct timespec start_time;
       clock_gettime(CLOCK_MONOTONIC, &start_time);
       
       // 요청 시작 추적
       track_request_start(&pool, server_idx);

       struct sockaddr_in target_addr;
       memset(&target_addr, 0, sizeof(target_addr));
       target_addr.sin_family = AF_INET;
       target_addr.sin_port = htons(server->port);
       target_addr.sin_addr.s_addr = inet_addr(server->address);

       bool request_success = true;
       if (connect(target_socket, (struct sockaddr*)&target_addr, sizeof(target_addr)) < 0) {
           log_message(LOG_ERROR, "Failed to connect to backend %s:%d", 
               server->address, server->port);
           request_success = false;
       } else {
           // 클라이언트로부터 데이터 수신 및 target 서버로 전달
           bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
           if (bytes_received > 0) {
               buffer[bytes_received] = '\0';
               if (send(target_socket, buffer, bytes_received, 0) < 0) {
                   log_message(LOG_ERROR, "Failed to send data to backend");
                   request_success = false;
               }
           } else if (bytes_received < 0) {
               log_message(LOG_ERROR, "Failed to receive data from client");
               request_success = false;
           }

           // target 서버로부터 응답 수신 및 클라이언트로 전달
           bytes_received = recv(target_socket, buffer, BUFFER_SIZE - 1, 0);
           if (bytes_received > 0) {
               buffer[bytes_received] = '\0';
               
               int status_code = 0;
               if (strstr(buffer, "200 OK")) {
                   status_code = 200;
               } else if (strstr(buffer, "404 Not Found")) {
                   status_code = 404;
               }
               
               // HTTP 응답 로깅
               log_http_response(client_ip, status_code, buffer);

               if (send(client_socket, buffer, bytes_received, 0) < 0) {
                   log_message(LOG_ERROR, "Failed to send response to client");
                   request_success = false;
               }
           } else if (bytes_received < 0) {
               log_message(LOG_ERROR, "Failed to receive response from backend");
               request_success = false;
           }
       }

       // 요청 종료 시간 계산
       struct timespec end_time;
       clock_gettime(CLOCK_MONOTONIC, &end_time);
       double response_time = (end_time.tv_sec - start_time.tv_sec) * 1000.0 + 
                            (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;

       // 요청 종료 추적
       track_request_end(&pool, server_idx, request_success, response_time);

       // 메트릭 로깅
       log_server_metrics(server->address, server->port,
           server->current_requests,
           server->total_requests,
           server->total_failures,
           server->avg_response_time);

       log_system_metrics(pool.total_requests,
           pool.total_failures,
           pool.avg_response_time);

       close(client_socket);
       close(target_socket);
   }

   close(listen_socket);
   return 0;
}