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
   
   // response_body에서 첫 줄만 추출
   char first_line[256] = {0};
   strncpy(first_line, response_body, sizeof(first_line) - 1);
   char* newline = strchr(first_line, '\n');
   if (newline) *newline = '\0';
   
   // JSON 부분 추출
   const char* json_start = strstr(response_body, "{");
   const char* json_end = strstr(response_body, "}");
   
   // JSON 있으면 JSON만, 없으면 첫 줄만 로깅
   if (json_start && json_end) {
       int json_length = json_end - json_start + 1;
       char json_part[1024] = {0};
       strncpy(json_part, json_start, json_length);
       log_message(level, "Client IP: %s, Status: %d, JSON: %s", 
                  client_ip, status_code, json_part);
   } else {
       log_message(level, "Client IP: %s, Status: %d, Response: %s", 
                  client_ip, status_code, first_line);
   }
}