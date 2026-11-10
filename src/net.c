#include "net.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "lz4.h"

#define DEFAULT_PORT "14500"
#define FLAG_YAML	0x04
#define FLAG_FLUSH	0x08
#define FLAG_RENCODEPLUS 0x10
#define COMP_LZ4	0x10
#define MAX_DECOMPRESSED (256u * 1024 * 1024)
#define DEFAULT_MAX_RECORD (16u * 1024 * 1024)

static int connect_unix(const char *path)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	if (strlen(path) >= sizeof(sa.sun_path)) {
		fprintf(stderr, "socket path too long: %s\n", path);
		return -1;
	}
	strcpy(sa.sun_path, path);
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa))) {
		fprintf(stderr, "cannot connect to %s: %s\n", path, strerror(errno));
		close(fd);
		return -1;
	}
	return fd;
}

static int connect_tcp(const char *hostport)
{
	char host[256];
	const char *port = DEFAULT_PORT;
	snprintf(host, sizeof(host), "%s", hostport);
	char *slash = strchr(host, '/');
	if (slash)
		*slash = 0;
	/* [ipv6]:port or host:port */
	char *h = host;
	if (*h == '[') {
		char *end = strchr(h, ']');
		if (!end)
			return -1;
		*end = 0;
		h++;
		if (end[1] == ':')
			port = end + 2;
	} else {
		char *colon = strrchr(h, ':');
		if (colon) {
			*colon = 0;
			port = colon + 1;
		}
	}
	struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res, *ai;
	int err = getaddrinfo(h, port, &hints, &res);
	if (err) {
		fprintf(stderr, "cannot resolve %s: %s\n", h, gai_strerror(err));
		return -1;
	}
	int fd = -1;
	for (ai = res; ai; ai = ai->ai_next) {
		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (!connect(fd, ai->ai_addr, ai->ai_addrlen))
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		fprintf(stderr, "cannot connect to %s port %s\n", h, port);
		return -1;
	}
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	return fd;
}

/* display number: $XDG_RUNTIME_DIR/xpra/N/socket, then the older ~/.xpra/HOSTNAME-N */
static int connect_display(const char *num)
{
	char path[512], host[256] = "";
	struct stat st;
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (rt) {
		snprintf(path, sizeof(path), "%s/xpra/%s/socket", rt, num);
		if (!stat(path, &st))
			return connect_unix(path);
	}
	gethostname(host, sizeof(host) - 1);
	const char *home = getenv("HOME");
	if (rt) {
		snprintf(path, sizeof(path), "%s/xpra/%s-%s", rt, host, num);
		if (!stat(path, &st))
			return connect_unix(path);
	}
	if (home) {
		snprintf(path, sizeof(path), "%s/.xpra/%s-%s", home, host, num);
		if (!stat(path, &st))
			return connect_unix(path);
	}
	fprintf(stderr, "no socket found for display :%s\n", num);
	return -1;
}

int net_connect(conn_t *c, const char *target)
{
	memset(c, 0, sizeof(*c));
	c->max_record = DEFAULT_MAX_RECORD;
	if (!strncmp(target, "tcp://", 6))
		c->fd = connect_tcp(target + 6);
	else if (!strncmp(target, "socket://", 9))
		c->fd = connect_unix(target + 9);
	else if (!strncmp(target, "socket:", 7))
		c->fd = connect_unix(target + 7);
	else if (target[0] == '/')
		c->fd = connect_unix(target);
	else if (target[0] == ':' && target[1])
		c->fd = connect_display(target + 1);
	else if (target[0] >= '0' && target[0] <= '9' && !strchr(target, '.') && !strchr(target, ':'))
		c->fd = connect_display(target);
	else
		c->fd = connect_tcp(target);
	return c->fd < 0 ? -1 : 0;
}

int conn_fill(conn_t *c)
{
	buf_reserve(&c->in, 65536);
	ssize_t n;
	do {
		n = read(c->fd, c->in.data + c->in.len, c->in.cap - c->in.len);
	} while (n < 0 && errno == EINTR);
	if (n <= 0)
		return -1;
	c->in.len += (size_t)n;
	return 0;
}

static void drop_chunks(conn_t *c)
{
	for (int i = 0; i < c->nchunks; i++)
		free(c->chunk_data[i]);
	c->nchunks = 0;
}

int conn_next_packet(conn_t *c, packet_t *p)
{
	for (;;) {
		if (c->in.len < 8)
			return 0;
		const uint8_t *h = c->in.data;
		uint8_t flags = h[1], comp = h[2], index = h[3];
		size_t size = (size_t)h[4] << 24 | (size_t)h[5] << 16 | (size_t)h[6] << 8 | h[7];
		if (h[0] != 'P') {
			fprintf(stderr, "invalid packet header\n");
			return -1;
		}
		if (index == 0) {
			if ((flags & ~(FLAG_FLUSH | FLAG_RENCODEPLUS | FLAG_YAML)) ||
			    (flags & (FLAG_RENCODEPLUS | FLAG_YAML)) != FLAG_RENCODEPLUS) {
				fprintf(stderr, "unsupported packet flags 0x%02x\n", flags);
				return -1;
			}
		} else if (flags || index > 15) {
			fprintf(stderr, "invalid chunk flags 0x%02x or index %u\n", flags, index);
			return -1;
		}
		if (comp && ((comp & 0xf0) != COMP_LZ4 || !(comp & 0x0f))) {
			fprintf(stderr, "unsupported compression 0x%02x\n", comp);
			return -1;
		}
		if (size > c->max_record) {
			fprintf(stderr, "packet too large: %zu bytes\n", size);
			return -1;
		}
		if (c->in.len < 8 + size)
			return 0;

		/* extract (and decompress) the payload */
		uint8_t *data;
		size_t len;
		if (comp) {
			data = lz4_decompress_sized(c->in.data + 8, size, MAX_DECOMPRESSED, &len);
			if (!data) {
				fprintf(stderr, "lz4 decompression failed\n");
				return -1;
			}
		} else {
			len = size;
			data = malloc(len ? len : 1);
			if (!data)
				return -1;
			memcpy(data, c->in.data + 8, len);
		}
		buf_consume(&c->in, 8 + size);

		if (index > 0) {
			if (c->nchunks >= MAX_CHUNKS) {
				free(data);
				fprintf(stderr, "too many raw chunks\n");
				return -1;
			}
			c->chunk_data[c->nchunks] = data;
			c->chunk_len[c->nchunks] = len;
			c->chunk_index[c->nchunks] = index;
			c->nchunks++;
			continue;
		}

		rv *v = rv_decode(data, len);
		if (!v || v->type != RV_LIST || !v->u.l.n || v->u.l.items[0].type != RV_STR) {
			fprintf(stderr, "invalid packet\n");
			rv_free(v);
			free(data);
			drop_chunks(c);
			return -1;
		}
		memset(p, 0, sizeof(*p));
		p->v = v;
		p->payload = data;
		/* substitute the raw chunks into their placeholders */
		for (int i = 0; i < c->nchunks; i++) {
			size_t idx = (size_t)c->chunk_index[i];
			if (idx >= v->u.l.n) {
				fprintf(stderr, "raw chunk index %zu outside the packet\n", idx);
				packet_free(p);
				drop_chunks(c);
				return -1;
			}
			rv *slot = &v->u.l.items[idx];
			rv *tmp = calloc(1, sizeof(rv));
			*tmp = *slot;
			rv_free(tmp);	/* release the placeholder contents */
			slot->type = RV_BYTES;
			slot->u.s.p = c->chunk_data[i];
			slot->u.s.len = c->chunk_len[i];
			p->chunks[p->nchunks++] = c->chunk_data[i];
		}
		c->nchunks = 0;
		return 1;
	}
}

void packet_free(packet_t *p)
{
	rv_free(p->v);
	free(p->payload);
	for (int i = 0; i < p->nchunks; i++)
		free(p->chunks[i]);
	memset(p, 0, sizeof(*p));
}

static int write_all(int fd, const uint8_t *p, size_t len)
{
	while (len) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

int conn_send(conn_t *c, const buf_t *payload)
{
	uint8_t h[8] = { 'P', FLAG_RENCODEPLUS, 0, 0 };
	h[4] = (uint8_t)(payload->len >> 24);
	h[5] = (uint8_t)(payload->len >> 16);
	h[6] = (uint8_t)(payload->len >> 8);
	h[7] = (uint8_t)payload->len;
	if (write_all(c->fd, h, 8) || write_all(c->fd, payload->data, payload->len))
		return -1;
	return 0;
}

void conn_close(conn_t *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
	drop_chunks(c);
	buf_free(&c->in);
}
