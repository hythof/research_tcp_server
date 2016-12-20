#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
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
static void unset_sending(rts_peer_t *peer) { peer->flags &= ~PEER_SENDING; }
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
  peer->send_iov = peer->send_fixed_iov;
  peer->send_offset = 0;
  peer->send_len = 0;
  peer->send_cap = sizeof(peer->send_fixed_iov) / sizeof(struct iovec);
  assert(peer->send_cap == 4);
  return peer;
}

static void init_peer(rts_peer_t *peer) {
  peer->flags = PEER_READING;
  peer->send_offset = 0;
  peer->send_len = 0;
}

static void free_peer(rts_peer_t *peer) {
  if (peer->send_iov != peer->send_fixed_iov) {
    free(peer->send_iov);
  }
  free(peer);
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
  int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  int on = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
    perror("setsockopt SO_REUSEADDR");
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
  freeaddrinfo(ai);
  return fd;

ERROR:
  freeaddrinfo(ai);
  return -1;
}

static int non_blocking_socket(int fd) {
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

static int try_accept(rts_thread_t *thread, rts_peer_t *peer) {
  const int listen_fd = thread->listen_fd;
  socklen_t addr_len = sizeof(struct sockaddr_storage);
  int fd = accept(listen_fd, (struct sockaddr *)&(peer->addr), &addr_len);
  if (fd < 0) {
    perror("accept");
    goto ERROR;
  }
  thread->stat.accept++;

  if (thread->num_connections >= thread->conf.num_max_connections) {
    thread->stat.close_max_connections++;
    goto ERROR;
  }

  if (non_blocking_socket(fd) < 0) {
    goto ERROR;
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
      // connection close by peer
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
      thread->stat.read_bytes += readed;
      thread->stat.read_count++;
      thread->conf.on_read(peer, buf, readed);
    }
  }
}

static void flush_send_buffer(rts_thread_t *thread, rts_peer_t *peer) {
  if (is_sending(peer)) {
    struct iovec *iov = peer->send_iov + peer->send_offset;
    size_t iov_len = peer->send_len - peer->send_offset;
    ssize_t writed = writev(peer->fd, iov, iov_len);

    if (writed < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // pass
      } else {
        perror("write");
        set_broken_send(peer);
        thread->conf.on_send_end(peer);
      }

    } else {
      thread->stat.write_bytes += writed;
      thread->stat.write_count++;
      size_t w = writed;
      while (1) {
        assert(peer->send_len > 0);
        struct iovec *iov = peer->send_iov + peer->send_offset;
        size_t len = iov->iov_len;
        if (len < w) {
          w -= len;
          ++peer->send_offset;
        } else {
          iov->iov_base = (char*)iov->iov_base + w;
          iov->iov_len -= w;
          if (iov->iov_len == 0) {
            unset_sending(peer);
            thread->conf.on_send_end(peer);
            peer->send_offset = 0;
          }
          break;
        }
      }
    }
  }
}

static void close_peer(rts_thread_t *thread, rts_peer_t *peer) {
  thread->conf.on_close(peer);
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
static void noop_on_read(rts_peer_t *peer, char *buf, size_t len) { rts_close(peer); UNUSE(buf); UNUSE(len); }
static void noop_on_close(rts_peer_t *peer) { UNUSE(peer); }
static void noop_on_send_end(rts_peer_t *peer) { UNUSE(peer); }
#undef UNUSE

// -- switch platform -------------------------------------
// TODO support kqueue and poll
#if EPOLL_CTL_ADD
#include "rts_epoll.h"
#endif

static void* spawn_server(void* arg) {
    rts_thread_t *thread = (rts_thread_t*)arg;
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
  for (int i=0; i<rts->conf.num_threads; ++i) {
    rts_thread_t *th = rts->threads + i;
    th->listen_fd = listen_fd;
    th->conf = rts->conf;
    th->read_buffer_length = rts->conf.num_read_buffer;
    th->read_buffer = malloc(rts->conf.num_read_buffer);
    th->pool_peer = malloc_peer();
  }

  rts->is_ready = 1;
  for (int i=1; i<rts->conf.num_threads; ++i) {
    rts_thread_t *th = rts->threads + i;
    if (pthread_create(&th->pthread, NULL, spawn_server, (void *)th) < 0) {
      perror("pthread_create");
    }
  }
  int ret = event_main(rts->threads);

  for (int i=1; i<rts->conf.num_threads; ++i) {
    rts_thread_t *th = rts->threads + i;
    if (pthread_join(th->pthread, NULL) < 0) {
      perror("pthread_join");
    }
  }
  rts->is_ready = 0;

  for (int i=0; i<rts->conf.num_threads; ++i) {
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

void rts_send(rts_peer_t *peer, void *buf, size_t length) {
  assert(peer->send_len <= peer->send_cap);
  assert(length > 0);
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
  iov->iov_len = length;
  ++peer->send_len;

  set_sending(peer);
}

void rts_close(rts_peer_t *peer) { set_close(peer); }

void rts_shutdown(rts_t *rts) {
  for(int i=0; i<rts->num_threads; ++i) {
    rts_thread_t *th = rts->threads + i;
    th->is_shutdown = 1;
  }
}

void rts_init(rts_t *rts) {
  memset(rts, 0, sizeof(rts_t));
  rts->conf.node = "*";
  rts->conf.service = "8888";
  rts->conf.backlog = 1024;
  rts->conf.num_threads = 8;
  rts->conf.num_read_buffer = 8192;
  rts->conf.num_read_timeout_ms = 1000;
  rts->conf.num_max_connections = 1000;
  rts->conf.on_connect = noop_on_connect;
  rts->conf.on_read = noop_on_read;
  rts->conf.on_send_end = noop_on_send_end;
  rts->conf.on_close = noop_on_close;
}

void rts_dump(FILE *stream, rts_t *rts) {
  rts_conf_t *c = &(rts->conf);
  rts_stat_t a;
  memset(&a, 0, sizeof(rts_stat_t));
  for (int i=0; i<rts->num_threads; ++i) {
    rts_thread_t *th = rts->threads + i;
    rts_stat_t *s = &(th->stat);
    a.accept += s->accept;
    a.read_count += s->read_count;
    a.write_count += s->write_count;
    a.read_bytes += s->read_bytes;
    a.write_bytes += s->write_bytes;
    a.close_normal += s->close_normal;
    a.close_by_peer += s->close_by_peer;
    a.close_error += s->close_error;
    a.close_max_connections += s->close_max_connections;
  }
  fprintf(stream,
          "-- rts statistics\n"
          "node                  %s\n"
          "service               %s\n"
          "accept                %llu\n"
          "read count            %llu\n"
          "write count           %llu\n"
          "read bytes            %llu\n"
          "write bytes           %llu\n"
          "close normal          %llu\n"
          "close by peer         %llu\n"
          "close error           %llu\n"
          "close max_connections %llu\n",
          c->node, c->service, a.accept, a.read_count, a.write_count,
          a.read_bytes, a.write_bytes, a.close_normal, a.close_by_peer,
          a.close_error, a.close_max_connections);
}
