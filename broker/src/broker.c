/* broker/src/broker.c  -- versión con buffers de salida y EPOLLOUT handling */
#define _GNU_SOURCE
#include "proto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>

/* epoll_fd definido en main.c */
extern int epoll_fd;

/* Roles */
typedef enum { ROLE_UNKNOWN=0, ROLE_PUBLISHER, ROLE_GATEWAY, ROLE_SUBSCRIBER } role_t;

/* Connection state machine */
typedef enum { S_AWAIT_LINE = 0, S_AWAIT_LEN, S_AWAIT_PAYLOAD } conn_state_t;

struct conn {
    int fd;
    role_t role;
    int authenticated;
    char node_id[64];

    /* input buffer */
    char inbuf[16384];
    size_t inbuf_len;

    /* state for incoming PUB */
    conn_state_t state;
    uint32_t expected_len;       /* payload length */
    char *payload_buf;           /* allocated expected_len+1 */
    uint32_t payload_received;   /* bytes received so far into payload_buf */
    char current_topic[256];

    /* OUTPUT buffer: queue pending data to send to this conn */
    char *outbuf;                /* allocated buffer */
    size_t outbuf_len;           /* total bytes in outbuf */
    size_t outbuf_sent;          /* bytes already sent */
};

/* fd_map global (visible to main.c as extern) */
struct conn *fd_map[MAX_FD_LIMIT];

/* Simple topic -> subscribers list (exact match) */
struct sub_node { int fd; struct sub_node *next; };
struct topic_entry { char *topic; struct sub_node *subs; struct topic_entry *next; };
static struct topic_entry *topics = NULL;

/* helpers */
struct conn *conn_create(int fd) {
    struct conn *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fd = fd;
    c->role = ROLE_UNKNOWN;
    c->authenticated = 0;
    c->inbuf_len = 0;
    c->state = S_AWAIT_LINE;
    c->expected_len = 0;
    c->payload_buf = NULL;
    c->payload_received = 0;
    c->current_topic[0] = '\0';
    c->outbuf = NULL;
    c->outbuf_len = 0;
    c->outbuf_sent = 0;
    if (fd >= 0 && fd < MAX_FD_LIMIT) fd_map[fd] = c;
    return c;
}

void conn_destroy(struct conn *c) {
    if (!c) return;
    if (c->payload_buf) free(c->payload_buf);
    if (c->outbuf) free(c->outbuf);
    int fd = c->fd;
    if (fd >= 0 && fd < MAX_FD_LIMIT) fd_map[fd] = NULL;
    free(c);
}

/* epoll modify helper: set/unset EPOLLOUT on a given fd based on want_out */
static int epoll_modify_events(int fd, int want_out) {
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN;
    if (want_out) ev.events |= EPOLLOUT;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == -1) {
        if (errno == ENOENT) {
            /* maybe fd not previously registered: add it */
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
                perror("epoll_ctl ADD in epoll_modify_events");
                return -1;
            }
            return 0;
        }
        perror("epoll_ctl MOD in epoll_modify_events");
        return -1;
    }
    return 0;
}

/* Queue data to connection's outbuf. Returns 0 success, -1 error (close connection) */
static int conn_queue_out(struct conn *c, const char *data, size_t len) {
    if (!c || len == 0) return 0;
    size_t need = c->outbuf_len - c->outbuf_sent + len;
    /* We store only the remaining bytes (not the already-sent prefix) */
    size_t remaining = c->outbuf_len - c->outbuf_sent;
    char *newbuf = malloc(remaining + len);
    if (!newbuf) return -1;
    /* copy remaining unsent data */
    if (remaining > 0) memcpy(newbuf, c->outbuf + c->outbuf_sent, remaining);
    /* append new data */
    memcpy(newbuf + remaining, data, len);
    /* free old buffer and replace */
    if (c->outbuf) free(c->outbuf);
    c->outbuf = newbuf;
    c->outbuf_len = remaining + len;
    c->outbuf_sent = 0;
    /* ensure EPOLLOUT is enabled for this fd */
    if (epoll_modify_events(c->fd, 1) < 0) {
        return -1;
    }
    return 0;
}

/* Try to flush outbuf to socket. Returns:
 *   0 -> flushed fully (no pending)
 *   1 -> still pending (would block)
 *  -1 -> fatal error (close)
 */
int flush_outbuf(int fd) {
    if (fd < 0 || fd >= MAX_FD_LIMIT) return -1;
    struct conn *c = fd_map[fd];
    if (!c) return -1;
    if (!c->outbuf || c->outbuf_sent >= c->outbuf_len) {
        /* nothing to send: ensure EPOLLOUT cleared */
        c->outbuf_len = 0;
        c->outbuf_sent = 0;
        if (c->outbuf) { free(c->outbuf); c->outbuf = NULL; }
        epoll_modify_events(fd, 0);
        return 0;
    }
    size_t to_send = c->outbuf_len - c->outbuf_sent;
    ssize_t w = write(fd, c->outbuf + c->outbuf_sent, to_send);
    if (w < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* socket not ready now */
            epoll_modify_events(fd, 1);
            return 1;
        }
        if (errno == EINTR) return 1;
        perror("write in flush_outbuf");
        return -1;
    }
    c->outbuf_sent += (size_t)w;
    if (c->outbuf_sent >= c->outbuf_len) {
        /* all sent */
        free(c->outbuf);
        c->outbuf = NULL;
        c->outbuf_len = 0;
        c->outbuf_sent = 0;
        /* remove EPOLLOUT interest */
        epoll_modify_events(fd, 0);
        return 0;
    }
    /* still pending */
    epoll_modify_events(fd, 1);
    return 1;
}

/* topic management (same as before) */
static struct topic_entry *find_topic(const char *topic) {
    for (struct topic_entry *t = topics; t; t = t->next)
        if (strcmp(t->topic, topic) == 0) return t;
    return NULL;
}

static struct topic_entry *create_topic(const char *topic) {
    struct topic_entry *t = malloc(sizeof(*t));
    if (!t) return NULL;
    t->topic = strdup(topic);
    t->subs = NULL;
    t->next = topics;
    topics = t;
    return t;
}

static int add_subscription(const char *topic, int fd) {
    struct topic_entry *t = find_topic(topic);
    if (!t) t = create_topic(topic);
    if (!t) return -1;
    for (struct sub_node *s = t->subs; s; s = s->next) if (s->fd == fd) return 0;
    struct sub_node *n = malloc(sizeof(*n));
    if (!n) return -1;
    n->fd = fd; n->next = t->subs; t->subs = n;
    return 0;
}

static void remove_fd_from_all(int fd) {
    struct topic_entry *t = topics;
    while (t) {
        struct sub_node **pp = &t->subs;
        while (*pp) {
            if ((*pp)->fd == fd) {
                struct sub_node *rem = *pp;
                *pp = rem->next;
                free(rem);
                continue;
            }
            pp = &(*pp)->next;
        }
        t = t->next;
    }
}

static void cleanup_empty_topics(void) {
    struct topic_entry **pt = &topics;
    while (*pt) {
        if ((*pt)->subs == NULL) {
            struct topic_entry *rem = *pt;
            *pt = rem->next;
            free(rem->topic);
            free(rem);
            continue;
        }
        pt = &(*pt)->next;
    }
}

/* Publish: enqueue 4-byte BE len + payload to each subscriber */
static void publish_to_topic(const char *topic, const char *payload, uint32_t len) {
    struct topic_entry *t = find_topic(topic);
    if (!t) {
        fprintf(stderr, "[INFO] publish: no subscribers for %s\n", topic);
        return;
    }
    int delivered = 0;
    struct sub_node **pp = &t->subs;
    while (*pp) {
        int fd = (*pp)->fd;
        if (fd < 0 || fd >= MAX_FD_LIMIT) {
            struct sub_node *rem = *pp; *pp = rem->next; free(rem); continue;
        }
        struct conn *c = fd_map[fd];
        if (!c) { struct sub_node *rem = *pp; *pp = rem->next; free(rem); continue; }
        /* prepare buffer 4-byte BE len + payload */
        size_t tot = sizeof(uint32_t) + len;
        char *buf = malloc(tot);
        if (!buf) { fprintf(stderr, "[WARN] OOM when publishing\n"); break; }
        uint32_t be = htonl(len);
        memcpy(buf, &be, sizeof(uint32_t));
        memcpy(buf + sizeof(uint32_t), payload, len);
        if (conn_queue_out(c, buf, tot) < 0) {
            fprintf(stderr, "[WARN] removing subscriber fd=%d (queue failed)\n", fd);
            free(buf);
            struct sub_node *rem = *pp; *pp = rem->next; free(rem);
            continue;
        }
        free(buf);
        delivered++;
        pp = &(*pp)->next;
    }
    fprintf(stderr, "[INFO] published topic=%s -> %d subscribers\n", topic, delivered);
}

/* Handle a parsed command line (no newline). Returns:
 *  0 success, 1 -> BYE (close), -1 error
 */
static int handle_command_line(struct conn *c, const char *line) {
    if (!c || !line) return -1;
    char cmd[TINY_MAX_LINE];
    strncpy(cmd, line, sizeof(cmd)-1); cmd[sizeof(cmd)-1] = '\0';
    char *save = NULL;
    char *tok = strtok_r(cmd, " ", &save);
    if (!tok) return -1;
    if (strcmp(tok, "HELLO") == 0) {
        char *role = strtok_r(NULL, " ", &save);
        char *node = strtok_r(NULL, " ", &save);
        if (!role || !node) { dprintf(c->fd, "ERR PROTO\n"); return -1; }
        if (strcmp(role, "PUBLISHER") == 0) c->role = ROLE_PUBLISHER;
        else if (strcmp(role, "GATEWAY") == 0) c->role = ROLE_GATEWAY;
        else if (strcmp(role, "SUBSCRIBER") == 0) c->role = ROLE_SUBSCRIBER;
        else c->role = ROLE_UNKNOWN;
        strncpy(c->node_id, node, sizeof(c->node_id)-1);
        c->authenticated = 1;
        dprintf(c->fd, "OK\n");
        fprintf(stderr, "[INFO] fd=%d HELLO role=%d node=%s\n", c->fd, c->role, c->node_id);
        return 0;
    } else if (strcmp(tok, "SUB") == 0) {
        char *topic = strtok_r(NULL, " ", &save);
        if (!topic) { dprintf(c->fd, "ERR PROTO\n"); return -1; }
        add_subscription(topic, c->fd);
        dprintf(c->fd, "OK\n");
        fprintf(stderr, "[INFO] fd=%d SUB %s\n", c->fd, topic);
        return 0;
    } else if (strcmp(tok, "UNSUB") == 0) {
        char *topic = strtok_r(NULL, " ", &save);
        if (!topic) { dprintf(c->fd, "ERR PROTO\n"); return -1; }
        struct topic_entry *t = find_topic(topic);
        if (t) {
            struct sub_node **ps = &t->subs;
            while (*ps) {
                if ((*ps)->fd == c->fd) { struct sub_node *rem = *ps; *ps = rem->next; free(rem); break; }
                ps = &(*ps)->next;
            }
        }
        dprintf(c->fd, "OK\n");
        fprintf(stderr, "[INFO] fd=%d UNSUB %s\n", c->fd, topic);
        return 0;
    } else if (strcmp(tok, "PUB") == 0) {
        char *topic = strtok_r(NULL, " ", &save);
        char *lenstr = strtok_r(NULL, " ", &save);
        if (!topic || !lenstr) { dprintf(c->fd, "ERR PROTO\n"); return -1; }
        long len = strtol(lenstr, NULL, 10);
        if (len <= 0 || len > TINY_MAX_PAYLOAD) { dprintf(c->fd, "ERR OVERFLOW\n"); return -1; }
        c->state = S_AWAIT_LEN;
        c->expected_len = (uint32_t)len;
        if (c->payload_buf) { free(c->payload_buf); c->payload_buf = NULL; }
        c->payload_buf = malloc(c->expected_len + 1 + sizeof(uint32_t));
        if (!c->payload_buf) { dprintf(c->fd, "ERR INTERNAL\n"); return -1; }
        c->payload_received = 0;
        strncpy(c->current_topic, topic, sizeof(c->current_topic)-1);
        c->current_topic[sizeof(c->current_topic)-1] = '\0';
        fprintf(stderr, "[INFO] fd=%d PUB header topic=%s expected_len=%u\n", c->fd, c->current_topic, c->expected_len);
        return 0;
    } else if (strcmp(tok, "PING") == 0) {
        dprintf(c->fd, "PONG\n"); return 0;
    } else if (strcmp(tok, "BYE") == 0) {
        dprintf(c->fd, "OK\n"); return 1;
    }
    dprintf(c->fd, "ERR PROTO\n");
    return -1;
}

/* Consume bytes from inbuf and process lines / payloads.
 * Return 0 ok, -1 error, -2 peer closed (request close)
 */
static int process_conn_incoming(struct conn *c) {
    if (!c) return -1;
    size_t pos = 0;
    while (pos < c->inbuf_len) {
        if (c->state == S_AWAIT_LINE) {
            char *nl = memchr(c->inbuf + pos, '\n', c->inbuf_len - pos);
            if (!nl) break;
            size_t linelen = (size_t)(nl - (c->inbuf + pos));
            if (linelen >= TINY_MAX_LINE) { fprintf(stderr, "[ERROR] line too long\n"); return -1; }
            char line[TINY_MAX_LINE];
            memcpy(line, c->inbuf + pos, linelen);
            line[linelen] = '\0';
            pos += linelen + 1;
            int h = handle_command_line(c, line);
            if (h == 1) return -2;
            if (h < 0) return -1;
            continue;
        } else if (c->state == S_AWAIT_LEN) {
            size_t need = sizeof(uint32_t) - c->payload_received;
            size_t avail = c->inbuf_len - pos;
            size_t to_copy = (avail < need) ? avail : need;
            memcpy(c->payload_buf + c->payload_received, c->inbuf + pos, to_copy);
            c->payload_received += to_copy;
            pos += to_copy;
            if (c->payload_received < sizeof(uint32_t)) break;
            uint32_t be = 0; memcpy(&be, c->payload_buf, sizeof(uint32_t));
            uint32_t declared = ntohl(be);
            if (declared != c->expected_len) {
                fprintf(stderr, "[ERROR] declared len %u != expected %u\n", declared, c->expected_len);
                return -1;
            }
            c->payload_received = 0;
            c->state = S_AWAIT_PAYLOAD;
            continue;
        } else if (c->state == S_AWAIT_PAYLOAD) {
            size_t need = c->expected_len - c->payload_received;
            size_t avail = c->inbuf_len - pos;
            size_t to_copy = (avail < need) ? avail : need;
            memcpy(c->payload_buf + c->payload_received, c->inbuf + pos, to_copy);
            c->payload_received += to_copy;
            pos += to_copy;
            if (c->payload_received < c->expected_len) break;
            c->payload_buf[c->expected_len] = '\0';
            publish_to_topic(c->current_topic, c->payload_buf, c->expected_len);
            free(c->payload_buf);
            c->payload_buf = NULL;
            c->expected_len = 0;
            c->payload_received = 0;
            c->current_topic[0] = '\0';
            c->state = S_AWAIT_LINE;
            continue;
        } else {
            fprintf(stderr, "[ERROR] unknown state\n");
            return -1;
        }
    }
    if (pos > 0) {
        if (pos < c->inbuf_len) memmove(c->inbuf, c->inbuf + pos, c->inbuf_len - pos);
        c->inbuf_len -= pos;
    }
    return 0;
}

/* Read available data into connection inbuf */
static int read_into_conn(struct conn *c) {
    if (!c) return -1;
    char tmp[4096];
    while (1) {
        ssize_t r = read(c->fd, tmp, sizeof(tmp));
        if (r == 0) return -2;
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("read");
            return -1;
        }
        if (c->inbuf_len + (size_t)r > sizeof(c->inbuf)) {
            fprintf(stderr, "[ERROR] inbuf overflow for fd=%d\n", c->fd);
            return -1;
        }
        memcpy(c->inbuf + c->inbuf_len, tmp, (size_t)r);
        c->inbuf_len += (size_t)r;
    }
    return 0;
}

/* Close and cleanup connection */
void close_connection(int fd) {
    if (fd < 0 || fd >= MAX_FD_LIMIT) return;
    struct conn *c = fd_map[fd];
    if (!c) return;
    fprintf(stderr, "[INFO] closing fd=%d\n", fd);
    remove_fd_from_all(fd);
    cleanup_empty_topics();
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1) {
        perror("epoll_ctl DEL");
    }
    close(fd);
    conn_destroy(c);
}

/* Accept loop */
int accept_new(int listen_fd) {
    while (1) {
        struct sockaddr_in addr;
        socklen_t alen = sizeof(addr);
        int client = accept(listen_fd, (struct sockaddr *)&addr, &alen);
        if (client < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            perror("accept");
            return -1;
        }
        if (client >= MAX_FD_LIMIT) { close(client); continue; }
        if (set_nonblocking(client) == -1) { perror("set_nonblocking"); close(client); continue; }
        struct conn *c = conn_create(client);
        if (!c) { close(client); continue; }
        struct epoll_event ev;
        ev.events = EPOLLIN; /* level-triggered for robustness */
        ev.data.fd = client;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client, &ev) == -1) {
            perror("epoll_ctl add client");
            close(client);
            conn_destroy(c);
            continue;
        }
        char addrbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, addrbuf, sizeof(addrbuf));
        fprintf(stderr, "[INFO] accepted fd=%d from %s:%d\n", client, addrbuf, ntohs(addr.sin_port));
    }
    return 0;
}

/* This function is called by main loop upon EPOLLIN for a fd */
int process_fd_event(int fd) {
    if (fd < 0 || fd >= MAX_FD_LIMIT) return -1;
    struct conn *c = fd_map[fd];
    if (!c) {
        fprintf(stderr, "[WARN] event for unknown fd=%d\n", fd);
        return -1;
    }
    int r = read_into_conn(c);
    if (r == -2) {
        /* Peer closed. Process any buffered data before closing */
        if (c->inbuf_len > 0) {
            int p = process_conn_incoming(c);
            if (p == -2) return -2;
            if (p < 0) return -1;
            return -2; /* peer closed after processing */
        } else {
            return -2;
        }
    } else if (r < 0) {
        return -1;
    }
    int p = process_conn_incoming(c);
    return p;
}
