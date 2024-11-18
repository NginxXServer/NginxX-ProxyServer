#include <stdio.h>
#include "proxy/proxy.h"

int main() {
    int listen_port = 39071;
    int target_port = 39067;
    char* target_host = "127.0.0.1";

    return run_proxy(listen_port, target_port, target_host);
}