#include <stdio.h>
#include "tcp_server.h"

int port = 8900;

static void server(void(*test)()) {
    conf_t conf;
    conf.node = "*";
    conf.service = "     ";
    conf.backlog = 1024;
    conf.num_read_buffer = 8192;
    conf.num_read_timeout = 1000;
    conf.num_max_connection = 1000;
    conf.on_connect
    test();
}

static void test_single() {

}

int main() {
    test_single();
}
