#include <stdio.h>

int main() {
    int listen_port = 39071;
    int target_port = 39067;
    char* target_host = "113.198.138.212";

    return run_proxy(listen_port, target_port, target_host);
}