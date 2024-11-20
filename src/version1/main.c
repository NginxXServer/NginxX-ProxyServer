#include <stdio.h>

extern int run_proxy(int listen_port, int target_port, char* target_host);

int main() {
    int listen_port = 39071;
    int target_port = 39020;
    char* target_host = "113.198.138.212";

    return run_proxy(listen_port, target_port, target_host);
}