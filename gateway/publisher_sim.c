// gateway/publisher_sim.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/socket.h>

#define GATEWAY_HOST "127.0.0.1"
#define GATEWAY_PORT 6000

static int connect_to_gateway(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(GATEWAY_PORT);
    inet_pton(AF_INET, GATEWAY_HOST, &addr.sin_addr);
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(s); return -1; }
    return s;
}

static int send_all(int fd, const void *buf, size_t n) {
    const char *p = buf; size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, p + sent, n - sent);
        if (w <= 0) return -1;
        sent += (size_t)w;
    }
    return 0;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    int s = connect_to_gateway();
    if (s < 0) { perror("connect gateway"); return 1; }
    /* say hello */
    send_all(s, "HELLO PUBLISHER sim-c\n", strlen("HELLO PUBLISHER sim-c\n"));
    /* read one line OK (simple) */
    char buf[256];
    ssize_t r = read(s, buf, sizeof(buf)-1);
    if (r > 0) { buf[r]=0; fprintf(stderr,"GOT: %s", buf); }
    /* now publish once (or loop) */
    for (int i=0;i<3;i++) {
        char payload[1024];
        time_t ts = time(NULL);
        int temp = 20 + (rand()%10);
        int hum = 30 + (rand()%40);
        int pl = snprintf(payload, sizeof(payload), "{\"node\":\"sim-c\",\"ts\":%ld,\"topic\":\"sensors/test/environment\",\"data\":{\"temp\":%d,\"hum\":%d}}", ts, temp, hum);
        char header[256];
        snprintf(header, sizeof(header), "PUB sensors/test/environment %d\n", pl);
        send_all(s, header, strlen(header));
        uint32_t be = htonl(pl);
        send_all(s, &be, sizeof(be));
        send_all(s, payload, pl);
        /* read OK */
        r = read(s, buf, sizeof(buf)-1);
        if (r > 0) { buf[r]=0; fprintf(stderr,"GOT: %s", buf); }
        sleep(1);
    }
    close(s);
    return 0;
}
