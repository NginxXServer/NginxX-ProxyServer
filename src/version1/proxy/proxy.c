#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define BUFFER_SIZE 4096

int run_proxy(int listen_port, int target_port, char* target_host) {
    int listen_socket, target_socket;
    struct sockaddr_in listen_addr, target_addr;
    char buffer[BUFFER_SIZE];
    int bytes_received;

    // 리스닝 소켓 생성
    listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_socket < 0) {
        perror("socket");
        return 1;
    }

    // 리스닝 소켓 주소 설정
    memset(&listen_addr, 0, sizeof(listen_addr));
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_port = htons(listen_port);
    listen_addr.sin_addr.s_addr = INADDR_ANY;

    // 리스닝 소켓 바인딩
    if (bind(listen_socket, (struct sockaddr*)&listen_addr, sizeof(listen_addr)) < 0) {
        perror("bind");
        close(listen_socket);
        return 1;
    }

    // 리스닝 모드 시작
    if (listen(listen_socket, 10) < 0) {
        perror("listen");
        close(listen_socket);
        return 1;
    }

    printf("Reverse proxy server listening on port %d\n", listen_port);

    while (1) {
        int client_socket = accept(listen_socket, NULL, NULL);
        if (client_socket < 0) {
            perror("accept");
            continue;
        }

        // 클라이언트 요청을 target 서버로 전달
        target_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (target_socket < 0) {
            perror("socket");
            close(client_socket);
            continue;
        }

        memset(&target_addr, 0, sizeof(target_addr));
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(target_port);
        target_addr.sin_addr.s_addr = inet_addr(target_host);

        if (connect(target_socket, (struct sockaddr*)&target_addr, sizeof(target_addr)) < 0) {
            perror("connect");
            close(client_socket);
            close(target_socket);
            continue;
        }

        // 클라이언트 요청을 target 서버로 전달
        while ((bytes_received = recv(client_socket, buffer, BUFFER_SIZE - 1, 0)) > 0) {
            buffer[bytes_received] = '\0';
            if (send(target_socket, buffer, bytes_received, 0) < 0) {
                perror("send");
                break;
            }
        }

        if (bytes_received < 0) {
            perror("recv");
        }

        // target 서버의 응답을 클라이언트로 전달
        while ((bytes_received = recv(target_socket, buffer, BUFFER_SIZE - 1, 0)) > 0) {
            buffer[bytes_received] = '\0';
            if (send(client_socket, buffer, bytes_received, 0) < 0) {
                perror("send");
                break;
            }
        }

        if (bytes_received < 0) {
            perror("recv");
        }

        close(client_socket);
        close(target_socket);
    }

    close(listen_socket);
    return 0;
}