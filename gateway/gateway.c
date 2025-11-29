/* gateway/gateway.c
   Gateway robusto: epoll non-blocking + queue for broker forwarding.
   Listens publishers on LISTEN_PORT and forwards PUB messages to broker (BROKER_HOST: BROKER_PORT).
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <time.h>

#define LISTEN_PORT 6000
#define BROKER_HOST "127.0.0.1"
#define BROKER_PORT 5000

#define MAX_EVENTS 128
#define MAX_LINE 1024
#define MAX_PAYLOAD 8192
#define MAX_CONN 10000
#define QUEUE_MAX_ITEMS 20000  /* global queue capacity to avoid unbounded memory use */

static volatile int keep_running = 1;
void int_handler(int s) { (void)s; keep_running = 0; }

/* helpers */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;
    return 0;
}

/* connection struct for each publisher */
typedef enum { C_AWAIT_LINE=0, C_AWAIT_LEN, C_AWAIT_PAYLOAD } conn_state_t;
struct conn {
    int fd;
    char inbuf[16384];
    size_t inbuf_len;

    conn_state_t state;
    uint32_t expected_len;
    char *payload_buf;
    uint32_t payload_received;
    char current_topic[256];

    /* outbuf for sending replies (OK / ERR etc) */
    char *outbuf;
    size_t outbuf_len;
    size_t outbuf_sent;

    struct conn *next; /* for bookkeeping if needed */
};

/* fd->conn map */
static struct conn *fd_map[MAX_CONN];

/* allocate/destroy connection */
static struct conn *conn_create(int fd) {
    struct conn *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fd = fd;
    c->inbuf_len = 0;
    c->state = C_AWAIT_LINE;
    c->expected_len = 0;
    c->payload_buf = NULL;
    c->payload_received = 0;
    c->current_topic[0] = '\0';
    c->outbuf = NULL;
    c->outbuf_len = 0;
    c->outbuf_sent = 0;
    if (fd >= 0 && fd < MAX_CONN) fd_map[fd] = c;
    return c;
}

static void conn_destroy(struct conn *c) {
    if (!c) return;
    if (c->payload_buf) free(c->payload_buf);
    if (c->outbuf) free(c->outbuf);
    int fd = c->fd;
    if (fd >= 0 && fd < MAX_CONN) fd_map[fd] = NULL;
    free(c);
}

/* Broker queue item */
struct mq_item {
    char *buf;     /* header + 4-byte + payload packed as contiguous bytes */
    size_t len;
    struct mq_item *next;
};

/* simple thread-safe queue (FIFO) */
struct mq {
    struct mq_item *head;
    struct mq_item *tail;
    size_t count;
    pthread_mutex_t lock;
    pthread_cond_t nonempty;
} msg_queue = {NULL, NULL, 0, PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER};

/* enqueue with limit */
static int mq_enqueue(char *buf, size_t len) {
    struct mq_item *it = malloc(sizeof(*it));
    if (!it) { free(buf); return -1; }
    it->buf = buf; it->len = len; it->next = NULL;
    pthread_mutex_lock(&msg_queue.lock);
    if (msg_queue.count >= QUEUE_MAX_ITEMS) {
        /* queue full: drop oldest to make space (or drop this new) - choose drop oldest */
        struct mq_item *old = msg_queue.head;
        if (old) {
            msg_queue.head = old->next;
            if (!msg_queue.head) msg_queue.tail = NULL;
            msg_queue.count--;
            free(old->buf);
            free(old);
        }
    }
    if (!msg_queue.tail) { msg_queue.head = it; msg_queue.tail = it; }
    else { msg_queue.tail->next = it; msg_queue.tail = it; }
    msg_queue.count++;
    pthread_cond_signal(&msg_queue.nonempty);
    pthread_mutex_unlock(&msg_queue.lock);
    return 0;
}

static struct mq_item *mq_dequeue_block() {
    pthread_mutex_lock(&msg_queue.lock);
    while (msg_queue.count == 0 && keep_running) pthread_cond_wait(&msg_queue.nonempty, &msg_queue.lock);
    if (!keep_running && msg_queue.count == 0) { pthread_mutex_unlock(&msg_queue.lock); return NULL; }
    struct mq_item *it = msg_queue.head;
    if (it) {
        msg_queue.head = it->next;
        if (!msg_queue.head) msg_queue.tail = NULL;
        msg_queue.count--;
    }
    pthread_mutex_unlock(&msg_queue.lock);
    return it;
}

/* epoll fd and listen fd globals */
static int epoll_fd = -1;
static int listen_fd = -1;

/* utility: modify epoll flags for fd (add/remove EPOLLOUT) */
static int epoll_modify(int fd, int want_out) {
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = EPOLLIN;
    if (want_out) ev.events |= EPOLLOUT;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == -1) {
        if (errno == ENOENT) {
            ev.events = EPOLLIN | (want_out ? EPOLLOUT : 0);
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
                perror("epoll_ctl ADD");
                return -1;
            }
            return 0;
        }
        perror("epoll_ctl MOD");
        return -1;
    }
    return 0;
}

/* flush outbuf for connection: tries to write pending bytes, handles EAGAIN */
static int flush_outbuf(struct conn *c) {
    if (!c) return -1;
    if (!c->outbuf || c->outbuf_len == 0) {
        /* nothing */
        return 0;
    }
    while (c->outbuf_sent < c->outbuf_len) {
        ssize_t w = write(c->fd, c->outbuf + c->outbuf_sent, c->outbuf_len - c->outbuf_sent);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                epoll_modify(c->fd, 1);
                return 1; /* pending */
            }
            if (errno == EINTR) continue;
            perror("write to publisher");
            return -1;
        }
        c->outbuf_sent += (size_t)w;
    }
    /* all sent */
    free(c->outbuf);
    c->outbuf = NULL;
    c->outbuf_len = 0;
    c->outbuf_sent = 0;
    epoll_modify(c->fd, 0);
    return 0;
}

/* queue reply (OK/ERR) to publisher connection */
static int conn_queue_reply(struct conn *c, const char *s) {
    size_t len = strlen(s);
    if (!c) return -1;
    if (!s) return -1;
    /* if there's no pending outbuf simply try to write immediately */
    if ((!c->outbuf || c->outbuf_len == 0) ) {
        ssize_t w = write(c->fd, s, len);
        if (w == (ssize_t)len) return 0;
        if (w >= 0) {
            /* partial write: allocate remainder */
            size_t rem = len - (size_t)w;
            char *nb = malloc(rem);
            if (!nb) return -1;
            memcpy(nb, s + w, rem);
            c->outbuf = nb; c->outbuf_len = rem; c->outbuf_sent = 0;
            epoll_modify(c->fd, 1);
            return 0;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* allocate full buffer */
                char *nb = malloc(len);
                if (!nb) return -1;
                memcpy(nb, s, len);
                c->outbuf = nb; c->outbuf_len = len; c->outbuf_sent = 0;
                epoll_modify(c->fd, 1);
                return 0;
            }
            perror("write immediate reply");
            return -1;
        }
    } else {
        /* append to existing outbuf */
        size_t remaining = c->outbuf_len - c->outbuf_sent;
        char *nb = malloc(remaining + len);
        if (!nb) return -1;
        memcpy(nb, c->outbuf + c->outbuf_sent, remaining);
        memcpy(nb + remaining, s, len);
        free(c->outbuf);
        c->outbuf = nb;
        c->outbuf_len = remaining + len;
        c->outbuf_sent = 0;
        epoll_modify(c->fd, 1);
        return 0;
    }
}

/* process incoming bytes in conn->inbuf (very similar to broker parsing) */
static int process_conn_incoming(struct conn *c) {
    if (!c) return -1;
    size_t pos = 0;
    while (pos < c->inbuf_len) {
        if (c->state == C_AWAIT_LINE) {
            char *nl = memchr(c->inbuf + pos, '\n', c->inbuf_len - pos);
            if (!nl) break;
            size_t linelen = (size_t)(nl - (c->inbuf + pos));
            if (linelen >= MAX_LINE) { fprintf(stderr, "[G] line too long\n"); return -1; }
            char line[MAX_LINE];
            memcpy(line, c->inbuf + pos, linelen);
            line[linelen] = '\0';
            pos += linelen + 1;
            /* parse */
            char *save = NULL;
            char *tok = strtok_r(line, " ", &save);
            if (!tok) continue;
            if (strcmp(tok, "HELLO") == 0) {
                /* reply OK */
                conn_queue_reply(c, "OK\n");
                continue;
            } else if (strcmp(tok, "PUB") == 0) {
                char *topic = strtok_r(NULL, " ", &save);
                char *lenstr = strtok_r(NULL, " ", &save);
                if (!topic || !lenstr) { conn_queue_reply(c, "ERR PROTO\n"); return -1; }
                long len = strtol(lenstr, NULL, 10);
                if (len <= 0 || len > MAX_PAYLOAD) { conn_queue_reply(c, "ERR OVERFLOW\n"); return -1; }
                c->state = C_AWAIT_LEN;
                c->expected_len = (uint32_t)len;
                if (c->payload_buf) { free(c->payload_buf); c->payload_buf = NULL; }
                c->payload_buf = malloc(c->expected_len + 1 + sizeof(uint32_t));
                if (!c->payload_buf) { conn_queue_reply(c, "ERR INTERNAL\n"); return -1; }
                c->payload_received = 0;
                strncpy(c->current_topic, topic, sizeof(c->current_topic)-1);
                c->current_topic[sizeof(c->current_topic)-1] = '\0';
                /* log */
                fprintf(stderr, "[G] fd=%d PUB header topic=%s expected_len=%u\n", c->fd, c->current_topic, c->expected_len);
                continue;
            } else {
                conn_queue_reply(c, "ERR PROTO\n");
                continue;
            }
        } else if (c->state == C_AWAIT_LEN) {
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
                fprintf(stderr, "[G] declared len mismatch %u != %u\n", declared, c->expected_len);
                conn_queue_reply(c, "ERR LEN\n");
                return -1;
            }
            c->payload_received = 0;
            c->state = C_AWAIT_PAYLOAD;
            continue;
        } else if (c->state == C_AWAIT_PAYLOAD) {
            size_t need = c->expected_len - c->payload_received;
            size_t avail = c->inbuf_len - pos;
            size_t to_copy = (avail < need) ? avail : need;
            memcpy(c->payload_buf + c->payload_received, c->inbuf + pos, to_copy);
            c->payload_received += to_copy;
            pos += to_copy;
            if (c->payload_received < c->expected_len) break;
            /* full payload ready; enqueue to broker queue as contiguous buffer: 4-byte BE + payload */
            c->payload_buf[c->expected_len] = '\0';
            size_t total = sizeof(uint32_t) + c->expected_len;
            char *pack = malloc(total);
            if (!pack) { conn_queue_reply(c, "ERR INTERNAL\n"); return -1; }
            uint32_t be = htonl(c->expected_len);
            memcpy(pack, &be, sizeof(uint32_t));
            memcpy(pack + sizeof(uint32_t), c->payload_buf, c->expected_len);
            /* We also need the header "PUB <topic> <len>\n" sent to broker BEFORE the 4-byte+payload.
             * Create header and combine into a single malloced buffer: header + body
             */
            char header[MAX_LINE];
            int hn = snprintf(header, sizeof(header), "PUB %s %u\n", c->current_topic, c->expected_len);
            if (hn < 0) { free(pack); conn_queue_reply(c, "ERR INTERNAL\n"); return -1; }
            size_t header_len = (size_t)hn;
            char *full = malloc(header_len + total);
            if (!full) { free(pack); conn_queue_reply(c, "ERR INTERNAL\n"); return -1; }
            memcpy(full, header, header_len);
            memcpy(full + header_len, pack, total);
            free(pack);
            /* enqueue to global queue */
            if (mq_enqueue(full, header_len + total) < 0) {
                free(full);
                conn_queue_reply(c, "ERR QUEUE\n");
                return -1;
            }
            /* reply OK to publisher (enqueue or immediate) */
            conn_queue_reply(c, "OK\n");
            fprintf(stderr, "[G] queued topic=%s len=%u from fd=%d\n", c->current_topic, c->expected_len, c->fd);
            /* reset state */
            free(c->payload_buf); c->payload_buf = NULL;
            c->expected_len = 0;
            c->payload_received = 0;
            c->current_topic[0] = '\0';
            c->state = C_AWAIT_LINE;
            continue;
        }
    }
    /* shift remaining bytes */
    if (pos > 0) {
        if (pos < c->inbuf_len) memmove(c->inbuf, c->inbuf + pos, c->inbuf_len - pos);
        c->inbuf_len -= pos;
    }
    return 0;
}

/* read into conn non-blocking */
static int read_into_conn(struct conn *c) {
    if (!c) return -1;
    char tmp[4096];
    while (1) {
        ssize_t r = read(c->fd, tmp, sizeof(tmp));
        if (r == 0) return -2;
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("read publisher");
            return -1;
        }
        if (c->inbuf_len + (size_t)r > sizeof(c->inbuf)) {
            fprintf(stderr, "[G] inbuf overflow fd=%d\n", c->fd);
            return -1;
        }
        memcpy(c->inbuf + c->inbuf_len, tmp, (size_t)r);
        c->inbuf_len += (size_t)r;
    }
    return 0;
}

/* close and cleanup connection */
static void close_conn_fd(int fd) {
    if (fd < 0 || fd >= MAX_CONN) return;
    struct conn *c = fd_map[fd];
    if (!c) return;
    fprintf(stderr, "[G] closing fd=%d\n", fd);
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1) {
        if (errno != ENOENT) perror("epoll del conn");
    }
    close(fd);
    conn_destroy(c);
}

/* accept loop */
static int accept_new(int listen_fd) {
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
        if (client >= MAX_CONN) { close(client); continue; }
        if (set_nonblocking(client) == -1) { perror("set_nonblocking"); close(client); continue; }
        struct conn *c = conn_create(client);
        if (!c) { close(client); continue; }
        struct epoll_event ev;
        ev.data.fd = client;
        ev.events = EPOLLIN;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client, &ev) == -1) {
            perror("epoll add client");
            close(client); conn_destroy(c); continue;
        }
        char addrbuf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.sin_addr, addrbuf, sizeof(addrbuf));
        fprintf(stderr, "[G] accepted fd=%d from %s:%d\n", client, addrbuf, ntohs(addr.sin_port));
    }
    return 0;
}

/* broker sender thread - single writer to broker */
static int broker_fd = -1;
static pthread_mutex_t broker_lock = PTHREAD_MUTEX_INITIALIZER;

static int connect_to_broker(void) {
    struct sockaddr_in addr;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    addr.sin_family = AF_INET; addr.sin_port = htons(BROKER_PORT);
    if (inet_pton(AF_INET, BROKER_HOST, &addr.sin_addr) <= 0) { close(s); return -1; }
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(s); return -1; }
    return s;
}

static int send_all_block(int fd, const void *buf, size_t len) {
    size_t sent = 0;
    const char *p = buf;
    while (sent < len) {
        ssize_t w = write(fd, p + sent, len - sent);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return 0;
}

static void *broker_sender(void *arg) {
    (void)arg;
    while (keep_running) {
        struct mq_item *it = mq_dequeue_block();
        if (!it) break; /* shutdown */
        /* ensure broker connected */
        while (keep_running) {
            if (broker_fd >= 0) break;
            int s = connect_to_broker();
            if (s >= 0) {
                broker_fd = s;
                fprintf(stderr, "[G] connected to broker %s:%d fd=%d\n", BROKER_HOST, BROKER_PORT, broker_fd);
                break;
            }
            fprintf(stderr, "[G] cannot connect to broker, retrying in 1s\n");
            sleep(1);
        }
        if (!keep_running) { free(it->buf); free(it); break; }
        /* send item to broker (blocking single writer) */
        pthread_mutex_lock(&broker_lock);
        if (broker_fd >= 0) {
            if (send_all_block(broker_fd, it->buf, it->len) < 0) {
                perror("send to broker");
                close(broker_fd); broker_fd = -1;
                /* re-enqueue the item at head? simple policy: drop it and continue */
                fprintf(stderr, "[G] dropped a message due to broker send error\n");
            } else {
                /* optionally read OK line from broker (non-blocking with short timeout) */
                /* we skip reading to keep throughput; broker responds OK to publishers not to gateway */
            }
        }
        pthread_mutex_unlock(&broker_lock);
        free(it->buf);
        free(it);
    }
    /* cleanup broker fd */
    if (broker_fd >= 0) { close(broker_fd); broker_fd = -1; }
    return NULL;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    signal(SIGINT, int_handler);
    signal(SIGTERM, int_handler);

    /* create listening socket */
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(LISTEN_PORT);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(listen_fd, 128) < 0) { perror("listen"); return 1; }
    if (set_nonblocking(listen_fd) < 0) { perror("set_nonblocking listen"); return 1; }

    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) { perror("epoll_create1"); return 1; }
    struct epoll_event ev;
    ev.data.fd = listen_fd; ev.events = EPOLLIN;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) { perror("epoll_ctl add listen"); return 1; }

    /* start broker sender thread */
    pthread_t broker_tid;
    if (pthread_create(&broker_tid, NULL, broker_sender, NULL) != 0) {
        perror("pthread_create");
        return 1;
    }

    fprintf(stderr, "[G] listening publishers on port %d\n", LISTEN_PORT);

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
            uint32_t evs = events[i].events;
            if (fd == listen_fd) {
                accept_new(listen_fd);
                continue;
            }
            struct conn *c = NULL;
            if (fd >= 0 && fd < MAX_CONN) c = fd_map[fd];
            if (!c) {
                fprintf(stderr, "[G] event for unknown fd=%d\n", fd);
                if (evs & (EPOLLHUP|EPOLLERR)) { if (fd >= 0) close(fd); }
                continue;
            }
            if (evs & (EPOLLHUP|EPOLLERR)) {
                close_conn_fd(fd);
                continue;
            }
            if (evs & EPOLLIN) {
                int rr = read_into_conn(c);
                if (rr == -2) { /* peer closed */
                    close_conn_fd(fd);
                    continue;
                } else if (rr < 0) {
                    close_conn_fd(fd);
                    continue;
                }
                int pr = process_conn_incoming(c);
                if (pr < 0) { close_conn_fd(fd); continue; }
            }
            if (evs & EPOLLOUT) {
                if (flush_outbuf(c) < 0) { close_conn_fd(fd); continue; }
            }
        }
    }

    /* shutdown */
    fprintf(stderr, "[G] shutting down\n");
    /* wake broker thread to finish */
    pthread_mutex_lock(&msg_queue.lock);
    keep_running = 0;
    pthread_cond_broadcast(&msg_queue.nonempty);
    pthread_mutex_unlock(&msg_queue.lock);
    pthread_join(broker_tid, NULL);

    if (listen_fd >= 0) close(listen_fd);
    if (epoll_fd >= 0) close(epoll_fd);
    /* close all conns */
    for (int i=0;i<MAX_CONN;i++) if (fd_map[i]) close_conn_fd(i);
    return 0;
}
