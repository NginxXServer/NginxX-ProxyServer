#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include "logger.h"

#define LOG_FILE "proxy_server.log"

void log_message(LogLevel level, const char* format, ...) {
   FILE* file = fopen(LOG_FILE, "ab");
   if (!file) return;

   time_t now = time(NULL);
   char time_buf[32];
   strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", localtime(&now));
   
   const char* level_str = (level == LOG_INFO) ? "INFO" : "ERROR";
   
   fprintf(file, "[%s][%s] ", time_buf, level_str);
   printf("[%s][%s] ", time_buf, level_str);
   
   va_list args, args_copy;
   va_start(args, format);
   va_copy(args_copy, args);

   vfprintf(file, format, args);
   fprintf(file, "\n");
   
   vprintf(format, args_copy);
   printf("\n");
   
   va_end(args);
   va_end(args_copy);
   
   fclose(file);
}

void log_http_response(const char* client_ip, int status_code, const char* response_body) {
   LogLevel level = (status_code >= 400) ? LOG_ERROR : LOG_INFO;
   
   log_message(level, "Client IP: %s, Status: %d, Response: %s", 
               client_ip, status_code, response_body);
}

// 서버 메트릭 로깅
void log_server_metrics(const char* server_addr, int port, int current_requests, 
                      int total_requests, int total_failures, double avg_response_time) {
   log_message(LOG_INFO, "[METRIC][SERVER %s:%d] Active: %d, Total: %d, Failures: %d, Avg Response: %.2fms",
       server_addr, port, current_requests, total_requests, total_failures, avg_response_time);
}

// 전체 시스템 메트릭 로깅
void log_system_metrics(int total_requests, int total_failures, double avg_response_time) {
   log_message(LOG_INFO, "[METRIC][SYSTEM] Total Requests: %d, Total Failures: %d, Avg Response: %.2fms",
       total_requests, total_failures, avg_response_time);
}

// 서버 상태 변경 로깅
void log_server_status_change(const char* server_addr, int port, bool is_healthy) {
   const char* status = is_healthy ? "healthy" : "unhealthy";
   log_message(LOG_INFO, "[STATUS] Server %s:%d marked as %s", 
       server_addr, port, status);
}