#ifndef __RTS_H__
#define __RTS_H__

#include <sys/socket.h>
#include <sys/uio.h>

typedef struct {
  void *user;
  int fd;
  int flags;
  struct iovec send_fixed_iov[4];
  struct iovec *send_iov;
  int send_offset;
  int send_len;
  int send_cap;
  unsigned int total_read_bytes;
  unsigned int total_write_bytes;
} rts_peer_t;

typedef struct {
  const char *node;
  const char *service;
  int backlog;
  int num_threads;
  int num_events;
  int num_read_buffer;
  int num_read_timeout_seconds;
  int num_max_connections;
  void (*on_connect)(rts_peer_t *peer);
  void (*on_read)(rts_peer_t *peer, char *buf, size_t length);
  void (*on_send_end)(rts_peer_t *peer);
  void (*on_close)(rts_peer_t *peer);
} rts_conf_t;

typedef struct {
  int current_connections;
  unsigned long long accept;
  unsigned long long read_count;
  unsigned long long write_count;
  unsigned long long read_bytes;
  unsigned long long write_bytes;
  unsigned long long close_normal;
  unsigned long long close_by_peer;
  unsigned long long close_error;
  unsigned long long close_max_connections;
  unsigned long long empty_read_connections;
  unsigned long long empty_write_connections;
} rts_stat_t;

typedef struct {
  rts_conf_t conf;
  pthread_t pthread;
  rts_stat_t stat;
  int listen_fd;
  char *read_buffer;
  size_t read_buffer_length;
  rts_peer_t *pool_peer;
  volatile int is_shutdown;
} rts_thread_t;

typedef struct {
  rts_conf_t conf;
  int is_ready;
  int num_threads;
  rts_thread_t *threads;
} rts_t;

void rts_init(rts_t *rts);
int rts_main(rts_t *rts);
void rts_stat(rts_t *rts, rts_stat_t *stat);
void rts_shutdown(rts_t *rts);
void rts_send(rts_peer_t *peer, void *buf, size_t length);
void rts_close(rts_peer_t *peer);
void rts_dump(FILE *stream, rts_t *rts);

#endif
