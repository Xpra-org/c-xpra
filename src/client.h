#ifndef CLIENT_H
#define CLIENT_H

#include "net.h"

typedef struct {
	const char *target;
	const char *password;	/* NULL: prompt on the terminal when challenged */
	int debug;
} client_opts;

int client_run(const client_opts *opts);

#endif
