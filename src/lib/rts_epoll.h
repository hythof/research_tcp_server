#ifndef __RTS_EPOLL_H__
#define __RTS_EPOLL_H__

typedef struct {
  int fd;
  struct epoll_event *events;
  size_t count;
} epoll_t;

static uint32_t events_by_peer(rts_peer_t *peer) {
  int sending = is_sending(peer);
  int reading = is_reading(peer);

  if (sending && reading) {
    return EPOLLIN | EPOLLOUT | EPOLLET;
  } else if (sending) {
    return EPOLLOUT | EPOLLET;
  } else if (reading) {
    return EPOLLIN | EPOLLET;
  } else {
    return 0;
  }
}

static void event_accept(rts_t *rts, epoll_t *epoll, struct epoll_event *ev) {
  int epoll_fd = epoll->fd;
  rts_peer_t *peer = rts->pool_peer;
  init_peer(peer);

  // accept
  int fd = try_accept(rts, peer);
  if (fd < 0) {
    return;
  }
  peer->fd = fd;

  // handle callback
  rts->conf.on_connect(peer);
  flush_send_buffer(rts, peer);

  // judge next events
  int32_t new_events = events_by_peer(peer);
  if (new_events == 0) {
    goto CLOSE;
  }
  ev->events = new_events;

  // epoll add
  ev->data.ptr = (void *)peer;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, ev) < 0) {
    perror("epoll_ctl: add");
    goto CLOSE;
  }

  // update status
  rts->num_connections++;
  rts->pool_peer = malloc_peer();

  return;
CLOSE:
  close_peer(rts, peer);
}

static void event_handle(rts_t *rts, epoll_t *epoll, struct epoll_event *ev) {
  int epoll_fd = epoll->fd;
  rts_peer_t *peer = (rts_peer_t *)ev->data.ptr;
  int fd = peer->fd;

  // handle callback
  if (ev->events & EPOLLIN) {
    fill_read_buffer(rts, fd, peer);
  }
  if (ev->events & EPOLLOUT) {
    flush_send_buffer(rts, peer);
  }

  // judge next events
  uint32_t new_events = events_by_peer(peer);
  if (new_events > 0) {
    ev->events = new_events;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, ev) < 0) {
      perror("epoll_ctl: mod");
      goto CLOSE;
    }
  } else {
    goto CLOSE;
  }

  return;

CLOSE:
  if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, ev) < 0) {
    perror("epll_ctl: del");
  }
  close_peer(rts, peer);
  free_peer(peer);
  rts->num_connections--;
}

int event_main(rts_t *rts) {
  const int event_count = 1024;

  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    perror("epoll_create1");
    return -1;
  }
  struct epoll_event *events = malloc(sizeof(struct epoll_event) * event_count);

  epoll_t epoll;
  epoll.fd = epoll_fd;
  epoll.events = events;
  epoll.count = event_count;

  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.ptr = NULL;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, rts->listen_fd, &ev)) {
    free(epoll.events);
    return -1;
  }

  while (!rts->is_shutdown) {
    int nfds = epoll_wait(epoll_fd, events, event_count, -1);
    if (nfds < 0) {
      if (rts->is_shutdown) {
        break;
      } else {
        perror("epoll_wait");
        break;
      }
    }
    for (int i = 0; i < nfds; ++i) {
      struct epoll_event *ev = &events[i];
      if (ev->data.ptr == NULL) {
        event_accept(rts, &epoll, ev);
      } else {
        event_handle(rts, &epoll, ev);
      }
    }
  }
  free(events);

  return 0;
}
#endif
