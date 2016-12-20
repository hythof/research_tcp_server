#ifndef __RTS_EPOLL_H__
#define __RTS_EPOLL_H__

pthread_mutex_t accept_mutex = PTHREAD_MUTEX_INITIALIZER;

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

static void event_accept(rts_thread_t *thread, epoll_t *epoll, struct epoll_event *ev) {
  int epoll_fd = epoll->fd;
  rts_peer_t *peer = thread->pool_peer;
  init_peer(peer);

  // accept
  pthread_mutex_lock(&accept_mutex);

  int fd = try_accept(thread, peer);
  if (fd < 0) {
    return;
  }
  peer->fd = fd;

  // handle callback
  thread->conf.on_connect(peer);
  flush_send_buffer(thread, peer);

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
  thread->num_connections++;
  thread->pool_peer = malloc_peer();

  pthread_mutex_unlock(&accept_mutex);
  return;
CLOSE:
  pthread_mutex_unlock(&accept_mutex);
  close_peer(thread, peer);
}

static void event_handle(rts_thread_t *thread, epoll_t *epoll, struct epoll_event *ev) {
  int epoll_fd = epoll->fd;
  rts_peer_t *peer = (rts_peer_t *)ev->data.ptr;
  int fd = peer->fd;

  // handle callback
  if (ev->events & EPOLLIN) {
    fill_read_buffer(thread, fd, peer);
  }
  flush_send_buffer(thread, peer);

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
  close_peer(thread, peer);
  free_peer(peer);
  thread->num_connections--;
}

int event_main(rts_thread_t *thread) {
  fflush(stdout);
  const int event_count = 128;

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
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, thread->listen_fd, &ev)) {
    free(epoll.events);
    return -1;
  }

  while (!thread->is_shutdown) {
    int nfds = epoll_wait(epoll_fd, events, event_count, -1);
    if (nfds < 0) {
      if (thread->is_shutdown) {
        break;
      } else {
        perror("epoll_wait");
        continue;
      }
    }
    for (int i = 0; i < nfds; ++i) {
      struct epoll_event *ev = &events[i];
      if (ev->data.ptr == NULL) {
        event_accept(thread, &epoll, ev);
      } else {
        event_handle(thread, &epoll, ev);
      }
    }
  }
  free(events);

  return 0;
}
#endif
