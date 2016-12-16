#ifndef __TCP_SERVER_H__
#define __TCP_SERVER_H__

#include <sys/socket.h>

typedef struct {
    void* user;
    int _internal_fd;
    int _internal_flags;
} peer_t;

typedef struct {
    const char *node;
    const char *service;
    int backlog;
    int num_threads;
    int num_read_buffer;
    int num_read_timeout;
    int num_max_connection;
    int(*on_connect)(peer_t* peer, struct sockaddr_storage* addr);
    int(*on_read)(peer_t* peer, char *buf, size_t length);
    void(*on_close)(peer_t* peer);
} conf_t;


void peer_send(peer_t* peer, char *buf, size_t length);
void peer_close(peer_t* peer);
int run_tcp_server(const conf_t* conf);

#endif
