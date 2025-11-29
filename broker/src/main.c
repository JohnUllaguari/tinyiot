#define _GNU_SOURCE
#include "proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* forward decls */
struct conn;
extern struct conn *fd_map[];

/* prototypes implemented in broker.c */
int accept_new(int listen_fd);
int process_fd_event(int fd);
void close_connection(int fd);
/* new: flush_outbuf called when EPOLLOUT */
int flush_outbuf(int fd);

/* global epoll fd used by broker.c */
int epoll_fd = -1;

static volatile int keep_running = 1;
void int_handler(int sig) { (void)sig; keep_running = 0; }

int create_and_bind(int port) {
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd == -1) { perror("socket"); return -1; }
    int opt = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(port); addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) { perror("bind"); close(sfd); return -1; }
    return sfd;
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    if (argc >= 2) port = atoi(argv[1]);

    signal(SIGINT, int_handler); signal(SIGTERM, int_handler);

    int listen_fd = create_and_bind(port);
    if (listen_fd < 0) return 1;
    if (set_nonblocking(listen_fd) == -1) { perror("set_nonblocking listen"); close(listen_fd); return 1; }
    if (listen(listen_fd, LISTEN_BACKLOG) == -1) { perror("listen"); close(listen_fd); return 1; }

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) { perror("epoll_create1"); close(listen_fd); return 1; }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) { perror("epoll_ctl add listen"); close(listen_fd); close(epoll_fd); return 1; }

    fprintf(stderr, "brokerd listening on port %d\n", port);

    const int MAX_EVENTS = 64;
    struct epoll_event events[MAX_EVENTS];

    while (keep_running) {
        int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            uint32_t evts = events[i].events;
            fprintf(stderr, "[DBG] epoll event fd=%d ev=0x%x\n", fd, evts);
            if (fd == listen_fd) {
                accept_new(listen_fd);
                continue;
            }
            if (evts & (EPOLLHUP | EPOLLERR)) {
                fprintf(stderr, "[INFO] epoll hangup/err on fd=%d\n", fd);
                close_connection(fd);
                continue;
            }
            if (evts & EPOLLIN) {
                int r = process_fd_event(fd);
                if (r == -2) {
                    close_connection(fd);
                } else if (r < 0) {
                    close_connection(fd);
                }
            }
            if (evts & EPOLLOUT) {
                int r = flush_outbuf(fd);
                if (r < 0) {
                    close_connection(fd);
                }
            }
        }
    }

    fprintf(stderr, "shutting down brokerd\n");
    close(listen_fd);
    close(epoll_fd);
    for (int i = 0; i < MAX_FD_LIMIT; ++i) if (fd_map[i]) close_connection(i);
    return 0;
}
