#ifndef PKTLINE_H
#define PKTLINE_H

/*
 * Silly packetized line writing interface
 */
void packet_flush(int fd);
void packet_write(int fd, const char *fmt, ...) __attribute__((format (printf, 2, 3)));

int packet_read_line(int fd, char *buffer, unsigned size);

#endif
