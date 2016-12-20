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

rts_t *rts = NULL;

static void on_read(rts_peer_t *peer, char *buf, size_t length) {
  char *echo = malloc(length);
  memcpy(echo, buf, length);
  rts_send(peer, echo, length);
}

static void handle_signal(int no) {
  if (no == SIGUSR1) {
    rts_shutdown(rts);
  }
}

static void run_echo_server(const char *service) {
  if (signal(SIGUSR1, handle_signal) == SIG_ERR) {
    perror("signal");
  }

  rts = rts_alloc();
  rts->conf.service = service;
  rts->conf.on_read = on_read;
  int ret = rts_main(rts);
  assert(ret == 0);
  rts_free(rts);
  rts = NULL;
}

static void *spawn_server(void *arg) {
  (void)arg;  // nowarn
  run_echo_server("8880");
  return NULL;
}

static void wait_tick() {
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 1000;
  nanosleep(&ts, &ts);
}

static void env(int (*f)(int)) {
  pthread_t pt;
  if (pthread_create(&pt, NULL, spawn_server, NULL) < 0) {
    perror("pthread_create");
    return;
  }
  while (rts == NULL || !rts->is_ready) {
    wait_tick();
  }

  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) {
    perror("socket");
    exit(1);
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_port = htons(8880);
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect()");
    shutdown(s, SHUT_RDWR);
    close(s);
    exit(2);
  }

  fputc(f(s) == 0 ? '.' : 'F', stdout);

  close(s);
  if (raise(SIGUSR1) < 0) {
    perror("kill");
  }
  if (pthread_join(pt, NULL) < 0) {
    perror("pthread_join");
  }
}

#define max(A, B) (A > B ? A : B)
#define min(A, B) (A < B ? A : B)

static int post(int fd, char *msg) {
  int len = strlen(msg) + 1;
  int offset = 0;
  int remain = len;
  while (remain > 0) {
    char *buf = msg + offset;
    int buf_len = len - offset;
    int writed = write(fd, buf, buf_len);
    if (writed == -1) {
      perror("write");
      return -1;
    } else {
      remain -= writed;
      offset += writed;
    }
  }
  return 0;
}

static int check(int fd, const char *msg) {
  int buf_len = strlen(msg) + 1;
  char *buf = malloc(buf_len);
  int offset = 0;
  int remain = buf_len;
  while (remain > 0) {
    int readed = read(fd, buf + offset, remain);
    if (readed == -1) {
      perror("write");
    } else if (readed == 0) {
      return -1;
    } else {
      remain -= readed;
      offset += readed;
    }
  }
  return memcmp(msg, buf, buf_len);
  ;
}
