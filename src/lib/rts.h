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
  struct sockaddr_storage addr;
} rts_peer_t;

typedef struct {
  const char *node;
  const char *service;
  int backlog;
  int num_read_buffer;
  int num_read_timeout_ms;
  int num_max_connections;
  void (*on_connect)(rts_peer_t *peer);
  void (*on_read)(rts_peer_t *peer, char *buf, size_t length);
  void (*on_send_end)(rts_peer_t *peer);
  void (*on_close)(rts_peer_t *peer);
} rts_conf_t;

typedef struct {
  unsigned long long accept;
  unsigned long long read_count;
  unsigned long long write_count;
  unsigned long long read_bytes;
  unsigned long long write_bytes;
  unsigned long long close_normal;
  unsigned long long close_by_peer;
  unsigned long long close_error;
  unsigned long long close_max_connections;
} rts_stat_t;

typedef struct {
  rts_conf_t conf;
  rts_stat_t stat;
  int listen_fd;
  int is_ready;
  int is_shutdown;
  int num_connections;
  char *read_buffer;
  size_t read_buffer_length;
  rts_peer_t *pool_peer;
} rts_t;

rts_t *rts_alloc();
void rts_free(rts_t *rts);
int rts_main(rts_t *rts);
void rts_shutdown(rts_t *rts);
void rts_send(rts_peer_t *peer, void *buf, size_t length);
void rts_close(rts_peer_t *peer);
void rts_dump(FILE *stream, rts_t *rts);

#endif
