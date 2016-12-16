#include <pthread.h>
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
#include "tcp_server.h"

#define PEER_CLOSE 0x01
#define PEER_READ  0x02
#define PEER_WRITE 0x04

static int try_listen(const char *node, const char *service, int backlog) {
    struct addrinfo hints;
    struct addrinfo* ai = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET6; // AF_INET6 accept both ipv4 and ipv6
    hints.ai_socktype = SOCK_STREAM; // TCP
    hints.ai_flags = AI_PASSIVE; // for bind()
    if (getaddrinfo(node, service, &hints, &ai) < 0) {
        perror("getaddrinfo");
        return -1;
    }
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    int on = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0) {
        perror("setsockopt SO_REUSEADDR");
        freeaddrinfo(ai);
        return -1;
    }
    if (bind(fd, ai->ai_addr, ai->ai_addrlen) < 0) {
        perror("bind");
        freeaddrinfo(ai);
        return -1;
    }
    if (listen(fd, backlog) < 0) {
        perror("listen");
        freeaddrinfo(ai);
        return -1;
    }
    freeaddrinfo(ai);
    return fd;
}

static int non_blocking_socket(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl");
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags) < 0) {
        perror("fcntl");
        return -1;
    }
    return 0;
}

static int try_accept(
    int listen_fd,
    peer_t *peer,
    int(*on_connect)(peer_t* peer, struct sockaddr_storage* addr)
) {
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    int fd = accept(listen_fd, (struct sockaddr*)&(addr), &addr_len);
    if (fd < 0) {
        perror("accept");
        close(fd);
        return -1;
    }
    if (non_blocking_socket(fd) < 0) {
        close(fd);
        return -1;
    }
    peer->_internal_fd = fd;
    if (on_connect(peer, &addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int read_buffer(
    int fd,
    char *buf,
    int buf_length,
    peer_t *peer,
    int(*on_read)(peer_t* peer, char *buf, size_t length)
) {
    while (1) {
        int len = read(fd, buf, buf_length);
        if (len == 0) {
            return -1; // connection close by peer
        } else if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return 0; // end of stream
            } else {
                perror("read");
                return -1;
            }
        } else {
            if (on_read(peer, buf, len) < 0) {
                return -1;
            } else {
                return 0;
            }
        }
    }
}

static int epoll_loop(int listen_fd, const conf_t* conf) {
    const int num_read_buffer = conf->num_read_buffer;
    const int num_max_connection = conf->num_max_connection;
    int(*on_connect)(peer_t* peer, struct sockaddr_storage* addr) = conf->on_connect;
    int(*on_read)(peer_t* peer, char *buf, size_t length) = conf->on_read;
    void(*on_close)(peer_t* peer) = conf->on_close;

    const int MAX_EVENTS = 1024;
    char *buf = malloc(num_read_buffer);
    struct epoll_event *events = malloc(sizeof(struct epoll_event) * MAX_EVENTS);
    struct epoll_event ev;
    const int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("epoll_create1");
        return -1;
    }
    ev.events = EPOLLIN;
    ev.data.ptr = NULL;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev)) {
        return -1;
    }

    int num_connections = 0;
    peer_t* new_peer = malloc(sizeof(peer_t));
    while(1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds < 0) {
            perror("epoll_wait");
            continue;
        }
        for (int i=0; i<nfds; ++i) {
            if (events[i].data.ptr == NULL) {
                int fd = try_accept(listen_fd, new_peer, on_connect);
                if (fd < 0) {
                    continue;
                }
                if (num_connections >= num_max_connection) {
                    close(fd);
                    continue;
                }
                ev.events = EPOLLIN | EPOLLET;
                ev.data.ptr = (void*)new_peer;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0) {
                    perror("epoll_ctl: add");
                    close(fd);
                }
                new_peer = malloc(sizeof(peer_t));
                ++num_connections;
            } else {
                peer_t *peer = (peer_t*)events[i].data.ptr;
                int fd = peer->_internal_fd;
                if (read_buffer(fd, buf, num_read_buffer, peer, on_read) < 0) {
                    on_close(peer);
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ev) < 0) {
                        perror("epll_ctl: delete");
                    }
                    if (close(fd) < 0) {
                        perror("close");
                    }
                    free(peer);
                    --num_connections;
                }
            }
        }
    }

    free(events);
    free(buf);
    free(new_peer);
    return 0;
}

int run_tcp_server(const conf_t* conf) {
    signal(SIGPIPE, SIG_IGN);
    int listen_fd = try_listen(conf->node, conf->service, conf->backlog);
    if (listen_fd < 0) {
        return -1;
    }
    return epoll_loop(listen_fd, conf);
}

void peer_send(peer_t* peer, char *buf, size_t length) {
    // TODO lazy write
    if(write(peer->_internal_fd, buf, length) < 0) {
        perror("write");
    }
}

void peer_close(peer_t* peer) {
    // TODO with lazy write
    peer->_internal_flags |= PEER_WRITE;
    if (close(peer->_internal_fd)) {
        perror("close");
    }
}
