#include <stdio.h>
#include <string.h>
#include <signal.h>
#include "rts.h"

#define UNUSE(X) (void) X

rts_t rts;
char response[] =
    "HTTP/1.1 200 OK\r\n"
    "Date: Thu, 29 Dec 2016 06:45:06 GMT\r\n"
    "Connection: close\r\n"
    "Content-Length: 2\r\n"
    "Server: rts\r\n"
    "content-type: text/html\r\n"
    "last-modified: Mon, 26 Dec 2016 01:18:49 GMT\r\n"
    "etag: 58606ff9-3\r\n"
    "accept-ranges: bytes\r\n"
    "\r\n"
    "ok";

int response_len;

static void on_read(rts_peer_t *peer, char *buf, size_t length) {
  UNUSE(buf);
  UNUSE(length);
  rts_send(peer, response, response_len);
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
  response_len = strlen(response);
  return run_http_server("8880");
}
