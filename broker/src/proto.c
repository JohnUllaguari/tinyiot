#define _GNU_SOURCE
#include "proto.h"
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdio.h>

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;
    return 0;
}

int read_nbytes_nb(int fd, void *buf, size_t n) {
    size_t done = 0;
    char *p = buf;
    while (done < n) {
        ssize_t r = read(fd, p + done, n - done);
        if (r == 0) return -2;
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -3;
            return -1;
        }
        done += (size_t)r;
    }
    return 0;
}

int write_nbytes_nb(int fd, const void *buf, size_t n) {
    size_t done = 0;
    const char *p = buf;
    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -3;
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}

int send_payload_nb(int fd, const char *json, uint32_t len) {
    if (!json) return -1;
    if (len == 0 || len > TINY_MAX_PAYLOAD) return -1;
    uint32_t be = htonl(len);
    int r = write_nbytes_nb(fd, &be, sizeof(be));
    if (r != 0) return r;
    return write_nbytes_nb(fd, json, len);
}

/* Read a line into buf; use recv(2) semantics with non-blocking awareness.
 * This is a simple helper for blocking-read contexts; for epoll loop we will
 * use per-connection inbuf instead. But keep it for tests.
 */
ssize_t read_line_nb(int fd, char *buf, size_t buflen) {
    size_t pos = 0;
    while (pos + 1 < buflen) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r == 0) {
            if (pos == 0) return -2;
            break;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return -3;
            return -1;
        }
        if (c == '\n') {
            buf[pos] = '\0';
            return (ssize_t)pos;
        } else {
            buf[pos++] = c;
        }
    }
    buf[pos] = '\0';
    return (ssize_t)pos;
}
