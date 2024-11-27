#include "health.h"
#include "../utils/logger.h"
#include <string.h>
#include <stdlib.h>

void init_backend_pool(struct backend_pool* pool) {
    pool->server_count = MAX_BACKENDS;
    pool->total_requests = 0;
    pool->total_failures = 0;
    pool->total_response_time = 0;
    pool->avg_response_time = 0;

    for (int i = 0; i < pool->server_count; i++) {
        struct backend_server* server = &pool->servers[i];
        server->address = BACKEND_ADDRESS;
        server->port = BASE_PORT + i;
        server->is_healthy = true;
        server->failed_responses = 0;
        server->current_requests = 0;
        server->total_requests = 0;
        server->total_failures = 0;
        server->total_response_time = 0;
        server->avg_response_time = 0;
        server->failure_rate = 0;
    }
}

// void cleanup_backend_pool(struct backend_pool* pool) {
   
// }

void track_request_start(struct backend_pool* pool, int server_idx) {
    struct backend_server* server = &pool->servers[server_idx];
    server->current_requests++;
    server->total_requests++;
    pool->total_requests++;
}

void track_request_end(struct backend_pool* pool, int server_idx, bool success, double response_time) {
    struct backend_server* server = &pool->servers[server_idx];
    
    server->current_requests--;
    if (!success) {
        server->total_failures++;
    }
    server->total_response_time += response_time;
    server->avg_response_time = server->total_response_time / server->total_requests;
    server->failure_rate = ((double)server->total_failures / server->total_requests) * 100;
    
    if (!success) {
        pool->total_failures++;
    }
    pool->total_response_time += response_time;
    pool->avg_response_time = pool->total_response_time / pool->total_requests;
    
    update_server_status(pool, server_idx, success);
}

void update_server_status(struct backend_pool* pool, int server_idx, bool request_success) {
    struct backend_server* server = &pool->servers[server_idx];
    
    if (!request_success) {
        server->failed_responses++;
        if (server->failed_responses >= MAX_FAILURES) {
            server->is_healthy = false;
        }
    } else {
        server->failed_responses = 0;
        server->is_healthy = true;
    }
}

bool is_server_available(struct backend_pool* pool, int server_idx) {
    return pool->servers[server_idx].is_healthy;
}