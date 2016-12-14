#ifndef __TCP_SERVER_H__
#define __TCP_SERVER_H__

#include <sys/socket.h>

typedef struct {
    void* user;
    int _internal_fd;
    int _internal_flags;
} rts_peer_t;

typedef struct {
    const char *node;
    const char *service;
    int backlog;
    int num_threads;
    int num_read_buffer;
    int num_read_timeout;
    int num_max_connection;
    void(*on_connect)(rts_peer_t* peer, struct sockaddr_storage* addr);
    void(*on_read)(rts_peer_t* peer, char *buf, size_t length);
    void(*on_close)(rts_peer_t* peer);
} rts_conf_t;

void rts_send(rts_peer_t* peer, char *buf, size_t length);
void rts_close(rts_peer_t* peer);
int rts_main(const rts_conf_t* conf);

#endif
