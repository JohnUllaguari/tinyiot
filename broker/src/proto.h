#ifndef TINYIOT_PROTO_H
#define TINYIOT_PROTO_H

#include <stdint.h>
#include <sys/types.h>

#define TINY_MAX_PAYLOAD 8192
#define TINY_MAX_LINE 1024
#define LISTEN_BACKLOG 128
#define DEFAULT_PORT 5000
#define MAX_FD_LIMIT 10000

int set_nonblocking(int fd);

/* Read exactly n bytes from fd into buf; non-blocking aware:
 *  0  success
 * -1  error
 * -2  EOF (peer closed)
 * -3  EAGAIN (would block)
 */
int read_nbytes_nb(int fd, void *buf, size_t n);
int write_nbytes_nb(int fd, const void *buf, size_t n);

/* send payload with 4-byte big-endian len prefix */
int send_payload_nb(int fd, const char *json, uint32_t len);

/* read a line terminated by '\n' into buf (buflen). Returns:
 * >=0 number of bytes read (without newline)
 * -1 error
 * -2 EOF (peer closed)
 * -3 incomplete (would block)
 */
ssize_t read_line_nb(int fd, char *buf, size_t buflen);

#endif
