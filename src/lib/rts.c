#include <features.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <error.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <limits.h>
#include "rts.h"

#define PEER_READING 0x01
#define PEER_SENDING 0x02
#define PEER_CLOSE 0x04
#define PEER_BROKEN_READ 0x08
#define PEER_BROKEN_SEND 0x10

static int is_broken_send(rts_peer_t *peer) {
  return peer->flags & PEER_BROKEN_SEND;
}
static int is_broken_read(rts_peer_t *peer) {
  return peer->flags & PEER_BROKEN_READ;
}
static int is_reading(rts_peer_t *peer) { return peer->flags & PEER_READING; }
static int is_sending(rts_peer_t *peer) { return peer->flags & PEER_SENDING; }
static int is_close(rts_peer_t *peer) { return peer->flags & PEER_CLOSE; }
static void unset_reading(rts_peer_t *peer) { peer->flags &= ~PEER_READING; }
static void unset_sending(rts_peer_t *peer) {
  peer->send_offset = 0;
  peer->send_len = 0;
  peer->flags &= ~PEER_SENDING;
}
static void set_broken_read(rts_peer_t *peer) {
  assert(!is_broken_read(peer));

  peer->flags |= PEER_BROKEN_READ;
  unset_reading(peer);
}
static void set_broken_send(rts_peer_t *peer) {
  assert(!is_broken_send(peer));

  peer->flags |= PEER_BROKEN_SEND;
  unset_sending(peer);
}
static void set_close(rts_peer_t *peer) {
  peer->flags |= PEER_CLOSE;
  unset_reading(peer);
}
static void set_sending(rts_peer_t *peer) {
  assert(!is_broken_send(peer));

  peer->flags |= PEER_SENDING;
}

static rts_peer_t *malloc_peer() {
  rts_peer_t *peer = malloc(sizeof(rts_peer_t));
  peer->flags = PEER_READING;
  peer->user = NULL;
  peer->send_iov = peer->send_fixed_iov;
  peer->send_offset = 0;
  peer->send_len = 0;
  peer->send_cap = sizeof(peer->send_fixed_iov) / sizeof(struct iovec);
  assert(peer->send_cap == 4);
  return peer;
}

static void init_peer(rts_peer_t *peer) {
  peer->flags = PEER_READING;
  peer->user = NULL;
  peer->send_offset = 0;
  peer->send_len = 0;
}

static void free_peer(rts_peer_t *peer) {
  if (peer->send_iov != peer->send_fixed_iov) {
    free(peer->send_iov);
  }
  free(peer);
}

static int set_non_blocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    perror("fcntl");
    return -1;
  }
  flags |= O_NONBLOCK;
  if (fcntl(fd, F_SETFL, flags) < 0) {
    perror("fcntl");
    return -1;
  }
  return 0;
}

static int try_listen(rts_t *rts) {
  const char *node = rts->conf.node;
  const char *service = rts->conf.service;
  int backlog = rts->conf.backlog;

  struct addrinfo hints;
  struct addrinfo *ai = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET6;       // AF_INET6 accept both ipv4 and ipv6
  hints.ai_socktype = SOCK_STREAM;  // TCP
  hints.ai_flags = AI_PASSIVE;      // for bind()
  if (getaddrinfo(node, service, &hints, &ai) < 0) {
    perror("getaddrinfo");
    return -1;
  }
  const int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  const int off = 0;
  const int on = 1;

  if (set_non_blocking(fd) < 0) {
    goto ERROR;
  }

  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
    perror("setsockopt SO_REUSEADDR");
    goto ERROR;
  }

  if (setsockopt(fd, SOL_IPV6, IPV6_V6ONLY, &off, sizeof(on)) < 0) {
    perror("setsockopt IPV6_V6ONLY");
    goto ERROR;
  }

  if (setsockopt(fd, SOL_TCP, TCP_DEFER_ACCEPT, &on, sizeof(on)) < 0) {
    perror("setsockopt TCP_DEFER_ACCEPT");
    goto ERROR;
  }

  if (bind(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
    perror("bind");
    goto ERROR;
  }

  if (listen(fd, backlog) < 0) {
    perror("listen");
    goto ERROR;
  }

  const int fast = 4096;
  if (setsockopt(fd, SOL_TCP, TCP_FASTOPEN, &fast, sizeof(fast)) < 0) {
    perror("setsockopt TCP_FASTOPEN");
    goto ERROR;
  }

  freeaddrinfo(ai);
  return fd;

ERROR:
  freeaddrinfo(ai);
  return -1;
}

static int try_accept(rts_thread_t *thread) {
  const int listen_fd = thread->listen_fd;
#if RTS_FEATURE_ACCEPT4
  int fd = accept4(listen_fd, NULL, NULL, O_NONBLOCK);
  if (fd < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // pass
    } else {
      perror("accept");
    }
    return -1;
  }
#else
  int fd = accept(listen_fd, NULL, NULL);
  if (fd < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // pass
    } else {
      perror("accept");
    }
    return -1;
  }
  if (set_non_blocking(fd) < 0) {
    goto ERROR;
  }
#endif

  thread->stat.accept++;

  if (thread->stat.current_connections >= thread->conf.num_max_connections) {
    thread->stat.close_max_connections++;
    goto ERROR;
  }

  const int on = 1;
  if (setsockopt(fd, SOL_TCP, TCP_NODELAY, &on, sizeof(on)) < 0) {
    perror("setsockopt: TCP_NODELAY");
  }
  if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) < 0) {
    perror("setsockopt: SO_KEEPALIVE");
  }
  const int timeout = thread->conf.num_read_timeout_seconds;
  if (setsockopt(fd, SOL_TCP, TCP_KEEPINTVL, &timeout, sizeof(timeout)) < 0) {
    perror("setsockopt: TCP_KEEPINTVL");
  }
  if (setsockopt(fd, SOL_TCP, TCP_KEEPIDLE, &timeout, sizeof(timeout)) < 0) {
    perror("setsockopt: TCP_KEEPIDLE");
  }

  return fd;

ERROR:
  close(fd);
  return -1;
}

static void fill_read_buffer(rts_thread_t *thread, int fd, rts_peer_t *peer) {
  char *buf = thread->read_buffer;
  size_t buf_length = thread->read_buffer_length;

  while (is_reading(peer)) {
    ssize_t readed = read(fd, buf, buf_length);
    if (readed == 0) {
      // peer connection close or shutdown read
      unset_reading(peer);
      return;
    } else if (readed < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // end of read buffer
        return;
      } else {
        perror("read");
        set_broken_read(peer);
        return;
      }
    } else {
      peer->total_read_bytes += readed;
      thread->stat.read_bytes += readed;
      thread->stat.read_count++;
      thread->conf.on_read(peer, buf, readed);
    }
  }
}

static void flush_send_buffer(rts_thread_t *thread, rts_peer_t *peer) {
  if (!is_sending(peer)) {
    return;
  }

  // writev
  struct iovec *iov = peer->send_iov + peer->send_offset;
  size_t iov_len = peer->send_len - peer->send_offset;
  iov_len = iov_len < IOV_MAX ? iov_len : IOV_MAX;
  ssize_t writed = writev(peer->fd, iov, iov_len);
  if (writed < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      // pass
    } else {
      perror("writev");
      set_broken_send(peer);
      thread->conf.on_send_end(peer);
    }
    return;
  }

  // update send parameter
  peer->total_write_bytes += writed;
  thread->stat.write_bytes += writed;
  thread->stat.write_count++;
  size_t w = writed;
  while (1) {
    assert(peer->send_len > 0);
    assert(w > 0);
    assert(peer->send_offset < peer->send_len);

    struct iovec *iov = peer->send_iov + peer->send_offset;
    size_t len = iov->iov_len;

    if (len < w) {
      w -= len;
      ++peer->send_offset;
      thread->conf.on_send_end(peer);
    } else if (len == w) {
      ++peer->send_offset;
      thread->conf.on_send_end(peer);
      if (peer->send_offset == peer->send_len) {
        unset_sending(peer);
      }
      break;
    } else {
      iov->iov_base = (char *)iov->iov_base + w;
      iov->iov_len -= w;
      break;
    }
  }
}

static void close_peer(rts_thread_t *thread, rts_peer_t *peer) {
  assert(is_sending(peer) == 0);
  assert(is_reading(peer) == 0);
  assert(peer->send_len == 0);

  thread->conf.on_close(peer);
  if (peer->total_read_bytes == 0) {
    thread->stat.empty_read_connections++;
  }
  if (peer->total_write_bytes == 0) {
    thread->stat.empty_write_connections++;
  }
  if (is_close(peer)) {
    thread->stat.close_normal++;
  } else if (is_reading(peer)) {
    thread->stat.close_error++;
  } else {
    thread->stat.close_by_peer++;
  }
  if (close(peer->fd) < 0) {
    perror("close");
  }
}

#define UNUSE(X) (void) X
static void noop_on_connect(rts_peer_t *peer) { UNUSE(peer); }
static void noop_on_read(rts_peer_t *peer, char *buf, size_t len) {
  rts_close(peer);
  UNUSE(buf);
  UNUSE(len);
}
static void noop_on_close(rts_peer_t *peer) { UNUSE(peer); }
static void noop_on_send_end(rts_peer_t *peer) { UNUSE(peer); }
#undef UNUSE

// -- switch platform -------------------------------------
// TODO support kqueue and poll
#include "rts_epoll.h"

static void *spawn_server(void *arg) {
  rts_thread_t *thread = (rts_thread_t *)arg;
  event_main(thread);
  return NULL;
}

// -- public interface ------------------------------------
int rts_main(rts_t *rts) {
  assert(rts->conf.num_threads > 0);

  signal(SIGPIPE, SIG_IGN);

  int listen_fd = try_listen(rts);
  if (listen_fd < 0) {
    return -1;
  }
  rts->num_threads = rts->conf.num_threads;
  rts->threads = malloc(sizeof(rts_thread_t) * rts->num_threads);
  memset(rts->threads, 0, sizeof(rts_thread_t) * rts->num_threads);
  for (int i = 0; i < rts->conf.num_threads; ++i) {
    rts_thread_t *th = rts->threads + i;
    th->listen_fd = listen_fd;
    th->conf = rts->conf;
    th->read_buffer_length = rts->conf.num_read_buffer;
    th->read_buffer = malloc(rts->conf.num_read_buffer);
    th->pool_peer = malloc_peer();
  }

  rts->is_ready = 1;
  for (int i = 1; i < rts->conf.num_threads; ++i) {
    rts_thread_t *th = rts->threads + i;
    if (pthread_create(&th->pthread, NULL, spawn_server, (void *)th) < 0) {
      perror("pthread_create");
    }
  }

  int ret = event_main(rts->threads);

  for (int i = 1; i < rts->conf.num_threads; ++i) {
    rts_thread_t *th = rts->threads + i;
    if (pthread_join(th->pthread, NULL) < 0) {
      perror("pthread_join");
    }
  }
  rts->is_ready = 0;

  for (int i = 0; i < rts->conf.num_threads; ++i) {
    rts_thread_t *th = rts->threads + i;
    free(th->read_buffer);
    free_peer(th->pool_peer);
  }
  free(rts->threads);

  if (close(listen_fd) < 0) {
    perror("close");
  }

  return ret;
}

void rts_send(rts_peer_t *peer, void *buf, size_t len) {
  assert(!is_broken_send(peer));
  assert(peer->send_len <= peer->send_cap);
  assert(len > 0);
  assert(buf != NULL);

  if (peer->send_len == peer->send_cap) {
    peer->send_cap = peer->send_len * 2;
    size_t new_iov_size = sizeof(struct iovec) * peer->send_cap;
    if (peer->send_fixed_iov == peer->send_iov) {
      peer->send_iov = malloc(new_iov_size);
      memcpy(peer->send_iov, peer->send_fixed_iov,
             sizeof(peer->send_fixed_iov));
    } else {
      peer->send_iov = realloc(peer->send_iov, new_iov_size);
    }
  }
  struct iovec *iov = peer->send_iov + peer->send_len;
  iov->iov_base = buf;
  iov->iov_len = len;
  ++peer->send_len;

  set_sending(peer);
}

void rts_close(rts_peer_t *peer) { set_close(peer); }

void rts_shutdown(rts_t *rts) {
  for (int i = 0; i < rts->num_threads; ++i) {
    rts_thread_t *th = rts->threads + i;
    th->is_shutdown = 1;
  }
}

void rts_init(rts_t *rts) {
  memset(rts, 0, sizeof(rts_t));
  rts->conf.node = "*";
  rts->conf.service = "8888";
  rts->conf.backlog = 65535;
  rts->conf.num_threads = 4;
  rts->conf.num_events = 256;
  rts->conf.num_write_buffer = 8192;
  rts->conf.num_read_buffer = 8192;
  rts->conf.num_read_timeout_seconds = 180;
  rts->conf.num_max_connections = 1000 * 1000;
  rts->conf.on_connect = noop_on_connect;
  rts->conf.on_read = noop_on_read;
  rts->conf.on_send_end = noop_on_send_end;
  rts->conf.on_close = noop_on_close;
}

void rts_stat(rts_t *rts, rts_stat_t *stat) {
  memset(stat, 0, sizeof(rts_stat_t));
  for (int i = 0; i < rts->num_threads; ++i) {
    rts_thread_t *th = rts->threads + i;
    rts_stat_t *s = &(th->stat);
    stat->current_connections += s->current_connections;
    stat->accept += s->accept;
    stat->read_count += s->read_count;
    stat->write_count += s->write_count;
    stat->read_bytes += s->read_bytes;
    stat->write_bytes += s->write_bytes;
    stat->close_normal += s->close_normal;
    stat->close_by_peer += s->close_by_peer;
    stat->close_error += s->close_error;
    stat->close_max_connections += s->close_max_connections;
    stat->empty_read_connections += s->empty_read_connections;
    stat->empty_write_connections += s->empty_write_connections;
  }
}

void rts_dump(FILE *stream, rts_t *rts) {
  rts_conf_t *c = &(rts->conf);
  rts_stat_t a;
  rts_stat(rts, &a);
  fprintf(stream,
          "-- rts statistics all\n"
          "node                    %s\n"
          "service                 %s\n"
          "current connections     %d\n"
          "accept                  %llu\n"
          "read count              %llu\n"
          "write count             %llu\n"
          "read bytes              %llu\n"
          "write bytes             %llu\n"
          "close normal            %llu\n"
          "close by peer           %llu\n"
          "close error             %llu\n"
          "close max_connections   %llu\n"
          "empty read connections  %llu\n"
          "empty write connections %llu\n"
          "accepts per thread     ",
          c->node, c->service, a.current_connections, a.accept, a.read_count,
          a.write_count, a.read_bytes, a.write_bytes, a.close_normal,
          a.close_by_peer, a.close_error, a.close_max_connections,
          a.empty_read_connections, a.empty_write_connections);
  for (int i = 0; i < rts->num_threads; ++i) {
    rts_thread_t *th = rts->threads + i;
    rts_stat_t *s = &(th->stat);
    fprintf(stream, " %llu", s->accept);
  }
  fprintf(stream, "\n");
}
