#include <stdio.h>
#include <string.h>
#include <signal.h>
#include "rts.h"

#define UNUSE(X) (void) X

rts_t rts;
char response[] =
  "HTTP/1.0 200 OK\r\n"
  "Date: Fri, 16 Dec 2016 03:51:21 GMT\r\n"
  "Server: rts\r\n"
  "Content-Length: 2\r\n"
  "Content-Type: text/plain; charset=utf-8\r\n"
  "\r\n"
  "ok";

static void on_read(rts_peer_t *peer, char *buf, size_t length) {
  UNUSE(buf);
  UNUSE(length);
  rts_send(peer, response, strlen(response));
  rts_close(peer);
}

static int run_http_server(const char *service) {
  rts_init(&rts);
  rts.conf.service = service;
  rts.conf.on_read = on_read;
  int ret = rts_main(&rts);
  rts_dump(stdout, &rts);
  return ret;
}

void handle_signal(int no) {
  if (no == SIGUSR1) {
    rts_shutdown(&rts);
  }
}

int main() {
  if (signal(SIGUSR1, handle_signal) == SIG_ERR) {
    perror("signal");
  }
  return run_http_server("8880");
}
