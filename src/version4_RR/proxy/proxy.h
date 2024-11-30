#ifndef PROXY_H
#define PROXY_H

#include <netinet/in.h>
int run_proxy(int listen_port);

void handle_connection(int client_fd, struct sockaddr_in client_addr);

int select_server(void);

#endif