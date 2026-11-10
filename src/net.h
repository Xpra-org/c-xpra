/* transport and record layer, see Network/Protocol.md section 3 */
#ifndef NET_H
#define NET_H

#include <stddef.h>
#include <stdint.h>

#include "buf.h"
#include "rencode.h"

#define MAX_CHUNKS 3

typedef struct {
	rv *v;			/* the decoded packet: a list */
	uint8_t *payload;	/* owns the bytes rv strings point into */
	uint8_t *chunks[MAX_CHUNKS];
	int nchunks;
} packet_t;

typedef struct {
	int fd;
	buf_t in;
	/* raw chunks waiting for their main packet */
	uint8_t *chunk_data[MAX_CHUNKS];
	size_t chunk_len[MAX_CHUNKS];
	int chunk_index[MAX_CHUNKS];
	int nchunks;
	size_t max_record;	/* declared payload limit */
} conn_t;

/* "tcp://host[:port]", "socket://path", "socket:path", "/path", ":N" or "N" */
int net_connect(conn_t *c, const char *target);
/* read what is available: returns -1 on EOF or error */
int conn_fill(conn_t *c);
/* returns 1 and sets *p when a complete packet is available, 0 if more data is needed, -1 on error */
int conn_next_packet(conn_t *c, packet_t *p);
void packet_free(packet_t *p);
/* send an encoded packet (the payload is a rencodeplus list) */
int conn_send(conn_t *c, const buf_t *payload);
void conn_close(conn_t *c);

#endif
