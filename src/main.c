#include "http_protocol.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>

#define NOUSE(X) (void)X

volatile int exit_flag = 0;

static void signal_handler(int no) {
    puts("signal_handler");
    if (no == SIGUSR1) {
        puts("normal exit");
        exit_flag = 1;
    }
}

static void* background_server(void* arg) {
    NOUSE(arg);
    run_http_server("*", "8880", 1024);
    return NULL;
}

int enable_profile() {
    if (signal(SIGUSR1, signal_handler)) {
        perror("signal");
        return -1;
    }
    pthread_t pt;
    if (pthread_create(&pt, NULL, background_server, NULL) < 0) {
        perror("pthread_create");
        return -1;
    }
    while (exit_flag == 0) {
        sleep(1);
    }
    return 0;
}

int main() {
    if (getenv("PROFILE") != NULL) {
        return enable_profile();
    } else {
        return run_http_server("*", "8880", 1024);
    }
}
