// gateway/gateway.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>

#define BACKLOG 32
#define LISTEN_PORT 6000
#define BROKER_HOST "127.0.0.1"
#define BROKER_PORT 5000
#define MAX_LINE 1024
#define MAX_PAYLOAD 8192
#define RECONNECT_WAIT 2

static volatile int keep_running = 1;
void int_handler(int s){ (void)s; keep_running = 0; }

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Simple blocking read_line (until '\n'). Returns len (without \n), -1 error, -2 EOF */
static ssize_t read_line_block(int fd, char *buf, size_t buflen) {
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
            return -1;
        }
        if (c == '\n') {
            buf[pos] = '\0';
            return (ssize_t)pos;
        } else buf[pos++] = c;
    }
    buf[pos] = '\0';
    return (ssize_t)pos;
}

/* Read exact n bytes blocking. Return 0 success, -1 error, -2 EOF */
static int read_nbytes_block(int fd, void *buf, size_t n) {
    size_t got = 0;
    char *p = buf;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return -2;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

/* Broker connection handling: persistent socket and mutex for serialized writes */
static int broker_fd = -1;
static pthread_mutex_t broker_lock = PTHREAD_MUTEX_INITIALIZER;

static int connect_to_broker(const char *host, int port) {
    struct sockaddr_in addr;
    int sfd;
    sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        close(sfd);
        return -1;
    }
    if (connect(sfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sfd);
        return -1;
    }
    return sfd;
}

/* Ensure broker_fd is connected; if not, try to reconnect (blocking) */
static int ensure_broker_conn(void) {
    while (keep_running) {
        if (broker_fd >= 0) return 0;
        int s = connect_to_broker(BROKER_HOST, BROKER_PORT);
        if (s >= 0) {
            broker_fd = s;
            fprintf(stderr, "[GATEWAY] connected to broker %s:%d fd=%d\n", BROKER_HOST, BROKER_PORT, broker_fd);
            return 0;
        }
        fprintf(stderr, "[GATEWAY] cannot connect to broker, retrying in %d s\n", RECONNECT_WAIT);
        sleep(RECONNECT_WAIT);
    }
    return -1;
}

/* Send exactly n bytes to broker (with mutex). Returns 0 success, -1 error */
static int broker_send_all(const void *buf, size_t n) {
    if (ensure_broker_conn() < 0) return -1;
    pthread_mutex_lock(&broker_lock);
    size_t sent = 0;
    const char *p = buf;
    while (sent < n) {
        ssize_t w = write(broker_fd, p + sent, n - sent);
        if (w <= 0) {
            if (errno == EINTR) continue;
            /* close broker_fd and set -1, so future attempts reconnect */
            close(broker_fd); broker_fd = -1;
            pthread_mutex_unlock(&broker_lock);
            return -1;
        }
        sent += (size_t)w;
    }
    pthread_mutex_unlock(&broker_lock);
    return 0;
}

static int broker_send_line_and_payload(const char *header_line, const void *payload, uint32_t payload_len) {
    /* header_line already ends with '\n' */
    if (broker_send_all(header_line, strlen(header_line)) < 0) return -1;
    uint32_t be = htonl(payload_len);
    if (broker_send_all(&be, sizeof(be)) < 0) return -1;
    if (broker_send_all(payload, payload_len) < 0) return -1;
    return 0;
}

/* Handle one publisher connection */
static void *publisher_thread(void *arg) {
    int cfd = (int)(intptr_t)arg;
    char line[MAX_LINE];
    /* read HELLO */
    ssize_t r = read_line_block(cfd, line, sizeof(line));
    if (r <= 0) {
        fprintf(stderr, "[GATEWAY] pub fd=%d HELLO read failed r=%zd\n", cfd, r);
        close(cfd); return NULL;
    }
    fprintf(stderr, "[GATEWAY] pub fd=%d -> %s\n", cfd, line);
    /* for now accept any HELLO (optionally validate) and reply OK */
    dprintf(cfd, "OK\n");

    /* read next lines: may be PUB <topic> <len>\n */
    while (keep_running) {
        ssize_t h = read_line_block(cfd, line, sizeof(line));
        if (h == -2) { fprintf(stderr, "[GATEWAY] pub fd=%d closed\n", cfd); break; }
        if (h < 0) { fprintf(stderr, "[GATEWAY] pub fd=%d read error\n", cfd); break; }
        if (h == 0) continue;
        /* parse */
        char cmd[MAX_LINE];
        strncpy(cmd, line, sizeof(cmd)-1); cmd[sizeof(cmd)-1]=0;
        char *save = NULL; char *tok = strtok_r(cmd, " ", &save);
        if (!tok) continue;
        if (strcmp(tok, "PUB") == 0) {
            char *topic = strtok_r(NULL, " ", &save);
            char *lenstr = strtok_r(NULL, " ", &save);
            if (!topic || !lenstr) { dprintf(cfd, "ERR PROTO\n"); continue; }
            long len = strtol(lenstr, NULL, 10);
            if (len <= 0 || len > MAX_PAYLOAD) { dprintf(cfd, "ERR OVERFLOW\n"); continue; }
            /* read 4-byte BE len */
            uint32_t be=0;
            if (read_nbytes_block(cfd, &be, sizeof(be)) != 0) { fprintf(stderr, "[GATEWAY] pub fd=%d EOF reading len\n", cfd); break; }
            uint32_t declared = ntohl(be);
            if ((long)declared != len) { fprintf(stderr, "[GATEWAY] len mismatch %u != %ld\n", declared, len); dprintf(cfd, "ERR LEN\n"); continue; }
            /* read payload */
            char *payload = malloc(len+1);
            if (!payload) { fprintf(stderr, "[GATEWAY] OOM\n"); dprintf(cfd, "ERR OOM\n"); break; }
            if (read_nbytes_block(cfd, payload, len) != 0) { fprintf(stderr, "[GATEWAY] pub fd=%d EOF reading payload\n", cfd); free(payload); break; }
            payload[len]=0;
            /* reply OK to publisher (optional) */
            dprintf(cfd, "OK\n");
            /* forward to broker */
            char header[MAX_LINE];
            int n = snprintf(header, sizeof(header), "PUB %s %ld\n", topic, len);
            if (n < 0) { free(payload); break; }
            if (broker_send_line_and_payload(header, payload, (uint32_t)len) < 0) {
                fprintf(stderr, "[GATEWAY] failed to forward to broker, payload dropped\n");
                /* proceed (we could buffer) */
            } else {
                fprintf(stderr, "[GATEWAY] forwarded topic=%s len=%ld\n", topic, len);
            }
            free(payload);
            continue;
        } else if (strcmp(tok, "HELLO") == 0) {
            /* already handled; respond OK */
            dprintf(cfd, "OK\n");
            continue;
        } else {
            dprintf(cfd, "ERR PROTO\n");
            continue;
        }
    }

    close(cfd);
    return NULL;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    signal(SIGINT, int_handler);
    signal(SIGTERM, int_handler);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(LISTEN_PORT);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listen_fd, BACKLOG) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "[GATEWAY] listening publishers on port %d\n", LISTEN_PORT);

    /* start connecting to broker in background (blocking reconnect attempts) */
    broker_fd = -1;
    if (ensure_broker_conn() < 0) { fprintf(stderr,"[GATEWAY] cannot connect to broker, exiting\n"); close(listen_fd); return 1; }

    while (keep_running) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        char peerbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, peerbuf, sizeof(peerbuf));
        fprintf(stderr, "[GATEWAY] accepted publisher fd=%d from %s:%d\n", cfd, peerbuf, ntohs(peer.sin_port));
        /* spawn thread to handle */
        pthread_t tid;
        if (pthread_create(&tid, NULL, publisher_thread, (void *)(intptr_t)cfd) != 0) {
            perror("pthread_create");
            close(cfd);
            continue;
        }
        pthread_detach(tid);
    }

    if (broker_fd >= 0) close(broker_fd);
    close(listen_fd);
    return 0;
}
