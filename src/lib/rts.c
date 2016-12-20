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
#include "rts.h"

#define PEER_READING     0x01
#define PEER_SENDING     0x02
#define PEER_CLOSE       0x04
#define PEER_BROKEN_READ 0x08
#define PEER_BROKEN_SEND 0x10

static void unset_reading(rts_peer_t* peer) {
    peer->flags &= ~PEER_READING;
}

static void unset_sending(rts_peer_t* peer) {
    peer->flags &= ~PEER_SENDING;
}

static void set_broken_read(rts_peer_t* peer) {
    peer->flags |= PEER_BROKEN_READ;
    unset_reading(peer);
}

static void set_broken_send(rts_peer_t* peer) {
    peer->flags |= PEER_BROKEN_SEND;
    unset_sending(peer);
}

static void set_close(rts_peer_t* peer) {
    peer->flags |= PEER_CLOSE;
    unset_reading(peer);
}

static void set_sending(rts_peer_t* peer) {
    peer->flags |= PEER_SENDING;
}

static int is_reading(rts_peer_t* peer) {
    return peer->flags & PEER_READING;
}

static int is_sending(const rts_peer_t* peer) {
    return peer->flags & PEER_SENDING;
}

static int is_close(rts_peer_t* peer) {
    return peer->flags & PEER_CLOSE;
}

static rts_peer_t* malloc_peer() {
    rts_peer_t* peer = malloc(sizeof(rts_peer_t));
    peer->flags = PEER_READING;
    peer->send_iov = peer->send_fixed_iov;
    peer->send_offset = 0;
    peer->send_len = 0;
    peer->send_cap = sizeof(peer->send_fixed_iov) / sizeof(struct iovec);
    assert(peer->send_cap == 4);
    return peer;
}

static void init_peer(rts_peer_t* peer) {
    peer->flags = PEER_READING;
    peer->send_offset = 0;
    peer->send_len = 0;
}

static int try_listen(rts_t* rts) {
    const char* node = rts->conf.node;
    const char* service = rts->conf.service;
    int backlog = rts->conf.backlog;

    struct addrinfo hints;
    struct addrinfo* ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6;      // AF_INET6 accept both ipv4 and ipv6
    hints.ai_socktype = SOCK_STREAM; // TCP
    hints.ai_flags = AI_PASSIVE;     // for bind()
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

static int try_accept(rts_t* rts, rts_peer_t *peer) {
    const int listen_fd = rts->listen_fd;
    socklen_t addr_len = sizeof(struct sockaddr_storage);
    int fd = accept(listen_fd, (struct sockaddr*)&(peer->addr), &addr_len);
    if (fd < 0) {
        perror("accept");
        goto ERROR;
    }
    rts->stat.accept++;

    if (rts->num_connections >= rts->conf.num_max_connections) {
        rts->stat.close_max_connections++;
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

static void fill_read_buffer(
    rts_t* rts,
    int fd,
    rts_peer_t *peer
) {
    char *buf = rts->read_buffer;
    size_t buf_length = rts->read_buffer_length;

    while (is_reading(peer)) {
        int len = read(fd, buf, buf_length);
        if (len == 0) {
            // connection close by peer
            unset_reading(peer);
            return;
        } else if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // end of read buffer
                return;
            } else {
                perror("read");
                set_broken_read(peer);
                return;
            }
        } else {
            rts->conf.on_read(peer, buf, len);
            rts->stat.read_bytes += len;
            rts->stat.read_count++;
        }
    }
}

static void flush_send_buffer(rts_t* rts, rts_peer_t* peer) {
    if (is_sending(peer)) {
        void* buf = peer->send_iov + peer->send_offset;
        size_t buf_len = peer->send_len;
        ssize_t writed = writev(peer->fd, buf, buf_len);
        if(writed < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // pass
            } else {
                perror("write");
                set_broken_send(peer);
                rts->conf.on_send_end(peer);
            }
        } else {
            rts->stat.write_bytes += writed;
            rts->stat.write_count++;

            size_t w = writed;
            while (1) {
                assert(peer->send_len > 0);
                struct iovec* iov = peer->send_iov + peer->send_offset;
                size_t len = iov->iov_len;
                if (len < w) {
                    w -= len;
                    ++peer->send_offset;
                    --peer->send_len;
                } else {
                    iov->iov_base += w;
                    iov->iov_len -= w;
                    if (iov->iov_len == 0) {
                        unset_sending(peer);
                        rts->conf.on_send_end(peer);
                        peer->send_offset = 0;
                        assert(peer->send_len == 1);
                    }
                    break;
                }
            }
        }
    }
}

static void close_peer(rts_t* rts, rts_peer_t* peer) {
    rts->conf.on_close(peer);
    if (is_close(peer)) {
        rts->stat.close_normal++;
    } else if (is_reading(peer)) {
        rts->stat.close_error++;
    } else {
        rts->stat.close_by_peer++;
    }
    if (close(peer->fd) < 0) {
        perror("close");
    }
}

#define UNUSE(X) (void)X
static void noop_on_connect(rts_peer_t* peer) {
    UNUSE(peer);
}
static void noop_on_read(rts_peer_t* peer, char *buf, size_t length) {
    rts_send(peer, buf, length);
    rts_close(peer);
}
static void noop_on_close(rts_peer_t* peer) {
    UNUSE(peer);
}
static void noop_on_send_end(rts_peer_t* peer) {
    UNUSE(peer);
}
#undef UNUSE

// -- switch platform -------------------------------------
// TODO support kqueue and poll
#if EPOLL_CTL_ADD
#include "rts_epoll.h"
#endif

// -- public interface ------------------------------------
int rts_main(rts_t* rts) {
    signal(SIGPIPE, SIG_IGN);
    int listen_fd = try_listen(rts);
    if (listen_fd < 0) {
        return -1;
    }
    rts->listen_fd = listen_fd;
    rts->read_buffer = malloc(rts->conf.num_read_buffer);
    rts->read_buffer_length = rts->conf.num_read_buffer;
    rts->pool_peer = malloc_peer();

    rts->is_ready = 1;
    int ret = event_main(rts);

    if (close(listen_fd) < 0) {
        perror("close");
    }
    return ret;
}

void rts_send(rts_peer_t* peer, void* buf, size_t length) {
    assert(peer->send_len <= peer->send_cap);
    assert(length > 0);
    assert(buf != NULL);
    if (peer->send_len == peer->send_cap) {
        size_t cap = sizeof(struct iovec) * peer->send_len * 2;
        peer->send_cap = cap;
        if (peer->send_fixed_iov == peer->send_iov) {
            peer->send_iov = malloc(cap);
            memcpy(peer->send_iov, peer->send_fixed_iov, sizeof(peer->send_fixed_iov));
        } else {
            peer->send_iov = realloc(peer->send_iov, cap);
        }
    }
    struct iovec* iov = peer->send_iov + peer->send_len;
    iov->iov_base = buf;
    iov->iov_len = length;
    ++peer->send_len;

    set_sending(peer);
}

void rts_close(rts_peer_t* peer) {
    set_close(peer);
}

void rts_shutdown(rts_t* rts) {
    rts->is_shutdown = 1;
}

rts_t* rts_alloc() {
    rts_t* rts = malloc(sizeof(rts_t));
    memset(rts, 0, sizeof(rts_t));
    rts->conf.node = "*";
    rts->conf.service = "8888";
    rts->conf.backlog = 1024;
    rts->conf.num_read_buffer = 8192;
    rts->conf.num_read_timeout_ms = 1000;
    rts->conf.num_max_connections = 1000;
    rts->conf.on_connect = noop_on_connect;
    rts->conf.on_read = noop_on_read;
    rts->conf.on_send_end = noop_on_send_end;
    rts->conf.on_close = noop_on_close;
    return rts;
}
void rts_free(rts_t* rts) {
    free(rts->read_buffer);
    free(rts->pool_peer);
    free(rts);
}
void rts_dump(FILE* stream, rts_t* rts) {
    rts_conf_t* c = &(rts->conf);
    rts_stat_t* s = &(rts->stat);
    fprintf(stream, "-- rts statistics\n"
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
"close max_connections %llu\n"
  , c->node
  , c->service
  , s->accept
  , s->read_count
  , s->write_count
  , s->read_bytes
  , s->write_bytes
  , s->close_normal
  , s->close_by_peer
  , s->close_error
  , s->close_max_connections
  );
}
