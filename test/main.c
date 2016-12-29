#include "test.h"

#define BIG_PACKET 1 * 1000 * 1000

static int send_recv(int fd, size_t size) {
  char *msg = malloc(size + 1);
  memset(msg, '.', size);
  msg[size] = '\0';
  return post(fd, msg) || recv_and_cmp(fd, msg);
}

static int test1(int fd) { return send_recv(fd, 0); }
static int test2(int fd) { return send_recv(fd, 1); }
static int test3(int fd) { return send_recv(fd, BIG_PACKET); }

int main() {
  run_test(test1, 1);
  check(stat.accept == 1);

  run_test(test2, 1);
  check(stat.accept == 1);
  check(stat.read_count == 1);
  check(stat.write_count == 1);
  check(stat.read_bytes == 1);
  check(stat.write_bytes == 1);
  check(stat.close_by_peer == 1);

  run_test(test3, 1);
  check(stat.accept == 1);
  check(stat.read_count > 1);
  check(stat.write_count >= 1);
  check(stat.read_bytes == BIG_PACKET);
  check(stat.write_bytes == BIG_PACKET);
  check(stat.close_by_peer == 1);

  run_test(test1, 2);
  check(stat.accept == 2);
  check(stat.close_by_peer == 2);

  run_test(test2, 2);
  check(stat.accept == 2);
  check(stat.read_count == 2);
  check(stat.write_count == 2);
  check(stat.read_bytes == 2);
  check(stat.write_bytes == 2);
  check(stat.close_by_peer == 2);

  puts("");

  return num_test_fail;
}
