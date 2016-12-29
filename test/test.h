#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "rts.h"

rts_t rts;
rts_stat_t stat;
int num_test_fail = 0;
int num_on_read = 0;
int num_on_send_end = 0;

static void on_read(rts_peer_t *peer, char *buf, size_t len) {
  char *echo = malloc(len);
  memcpy(echo, buf, len);
  rts_send(peer, echo, len);
  ++num_on_read;
}

static void on_send_end(rts_peer_t *peer) {
  (void)peer;
  ++num_on_send_end;
}

static void handle_signal(int no) {
  if (no == SIGUSR1) {
    rts_shutdown(&rts);
  }
}

static void run_echo_server(const char *service) {
  if (signal(SIGUSR1, handle_signal) == SIG_ERR) {
    perror("test signal");
  }

  rts_init(&rts);
  rts.conf.service = service;
  rts.conf.on_read = on_read;
  rts.conf.on_send_end = on_send_end;
  rts.conf.num_max_connections = 2;
  int ret = rts_main(&rts);
  assert(ret == 0);
}

static void *spawn_server(void *arg) {
  (void)arg;  // nowarn
  run_echo_server("8880");
  return NULL;
}

static int do_connect() {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    perror("test socket");
    exit(1);
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_port = htons(8880);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("test connect");
    close(s);
    exit(2);
  }
  return s;
}

static void wait_tick() {
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 1000;
  nanosleep(&ts, &ts);
}

#define check(COND) check_core(COND, __FILE__, __LINE__)
static void check_core(int cond, const char *file, int line) {
  if (cond) {
    fputc('.', stdout);
  } else {
    fputc('F', stdout);
    ++num_test_fail;
    fprintf(stderr, "-- test fail %s %d\n", file, line);
    rts_dump(stderr, &rts);
    fflush(stdout);
    fflush(stderr);
  }
}

static void run_test_core(int (*f)(int), int n) {
  pthread_t pt;
  if (pthread_create(&pt, NULL, spawn_server, NULL) < 0) {
    perror("test pthread_create");
    exit(-1);
  }
  while (!rts.is_ready) {
    wait_tick();
  }

  int fds[n];
  for (int i = 0; i < n; ++i) {
    fds[i] = do_connect();
  }
  for (int i = 0; i < n; ++i) {
    int s = fds[i];
    check(f(s) == 0);
  }

  for (int i = 0; i < n; ++i) {
    int s = fds[i];
    close(s);
  }

  while (1) {
    rts_stat(&rts, &stat);
    if (stat.accept == (unsigned int)n && stat.current_connections == 0 &&
        stat.close_by_peer == (unsigned int)n) {
      break;
    }
    wait_tick();
  }

  if (raise(SIGUSR1) < 0) {
    perror("test raise");
  }
  if (pthread_join(pt, NULL) < 0) {
    perror("test pthread_join");
  }

  check(num_on_read == num_on_send_end);
}
static void run_test(int (*f)(int), int n) {
  int backup_num_max_connections = rts.conf.num_max_connections;
  rts.conf.num_max_connections = n;
  run_test_core(f, n);
  rts.conf.num_max_connections = backup_num_max_connections;
}

static int post(int fd, char *msg) {
  int len = strlen(msg);
  while (len > 0) {
    int writed = write(fd, msg, len);
    if (writed == -1) {
      perror("test write");
      return -1;
    } else {
      msg += writed;
      len -= writed;
    }
  }
  return 0;
}

static int recv_and_cmp(int fd, const char *msg) {
  size_t len = strlen(msg);
  size_t rest = len;
  char *buf = malloc(len + 1);
  memset(buf, 0, len + 1);
  while (rest > 0) {
    int readed = read(fd, buf + (len - rest), rest);
    if (readed == -1) {
      perror("test read");
    } else if (readed == 0) {
      return -1;
    } else {
      rest -= readed;
    }
  }
  return memcmp(msg, buf, len);
}
