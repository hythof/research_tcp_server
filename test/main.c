#include <stdio.h>
#include "test.h"

static const int port = 8900;

static int send_recv(int fd, size_t size) {
    char* msg = malloc(size + 1);
    memset(msg, 'a', size);
    msg[size - 1] = 'Z';
    msg[size] = '\0';
    return post(fd, msg)
        || check(fd, msg);
}

static int test1(int fd) {
    return send_recv(fd, 1);
}

static int test2(int fd) {
    return send_recv(fd, 1 * 1000);
}

int main() {
    env(test1);
    env(test2);
    puts("");
}
