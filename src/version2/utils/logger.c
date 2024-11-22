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