#include "test.h"

#define BIG_PACKET 100 * 1000 * 1000

static int send_recv(int fd, size_t size) {
  char *msg = malloc(size + 1);
  memset(msg, '.', size);
  msg[size] = '\0';
  return post(fd, msg) || recv_and_cmp(fd, msg);
}

static int case_empty(int fd) { return send_recv(fd, 0); }
static int case_one(int fd) { return send_recv(fd, 1); }
static int case_big(int fd) { return send_recv(fd, BIG_PACKET); }

static void test_empty(unsigned int count) {
  run_test(case_empty, count);
  check(stat.accept == count);
  check(stat.read_count == 0);
  check(stat.write_count == 0);
  check(stat.read_bytes == 0);
  check(stat.write_bytes == 0);
  check(stat.close_by_peer == count);
}

static void test_one(unsigned int count) {
  run_test(case_one, count);
  check(stat.accept == count);
  check(stat.read_count == count);
  check(stat.write_count == count);
  check(stat.read_bytes == count);
  check(stat.write_bytes == count);
  check(stat.close_by_peer == count);
}

static void test_big_packet(unsigned int count) {
  run_test(case_big, count);
  check(stat.accept == count);
  check(stat.read_count > 1 * count);
  check(stat.write_count > 1 * count);
  check(stat.read_bytes == BIG_PACKET * count);
  check(stat.write_bytes == BIG_PACKET * count);
  check(stat.close_by_peer == count);
}

int main() {
  test_empty(1);
  test_empty(2);
  test_one(1);
  test_one(2);
  test_big_packet(1);
  test_big_packet(2);

  test_one(500);
  puts("");

  return num_test_fail;
}
