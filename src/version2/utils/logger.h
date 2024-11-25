#ifndef LOGGER_H
#define LOGGER_H

#include <stdbool.h>

typedef enum {
    LOG_INFO,
    LOG_ERROR
} LogLevel;

void log_message(LogLevel level, const char* format, ...);
void log_http_response(const char* client_ip, int status_code, const char* response_body);

void log_server_metrics(const char* server_addr, int port, int current_requests, 
                       int total_requests, int total_failures, double avg_response_time);
void log_system_metrics(int total_requests, int total_failures, double avg_response_time);
void log_server_status_change(const char* server_addr, int port, bool is_healthy);

#endif