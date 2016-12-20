#include <stdio.h>
#include "test.h"

static int send_recv(int fd, size_t size) {
  char *msg = malloc(size + 1);
  memset(msg, '.', size);
  msg[size] = '\0';
  return post(fd, msg) || check(fd, msg);
}

static int test1(int fd) { return send_recv(fd, 0); }
static int test2(int fd) { return send_recv(fd, 1); }
static int test3(int fd) { return send_recv(fd, 80 * 1000); }

int main() {
  run(test1, 1);
  run(test2, 1);
  run(test3, 1);
  run(test1, rts.conf.num_max_connections);
  run(test2, rts.conf.num_max_connections);
  puts("");
}
