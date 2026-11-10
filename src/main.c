/* c-xpra: a minimal xpra client */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-d] [--password-file=FILE] TARGET\n"
		"  TARGET: tcp://host[:port], host:port, socket:/path, /path, :DISPLAY\n"
		"  the password can also be given in the XPRA_PASSWORD environment variable\n",
		argv0);
}

static char *read_file(const char *path)
{
	static char pw[1024];
	FILE *f = fopen(path, "r");
	if (!f) {
		perror(path);
		return NULL;
	}
	size_t n = fread(pw, 1, sizeof(pw) - 1, f);
	fclose(f);
	pw[n] = 0;
	pw[strcspn(pw, "\r\n")] = 0;
	return pw;
}

int main(int argc, char **argv)
{
	client_opts opts = { 0 };
	opts.password = getenv("XPRA_PASSWORD");
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (!strcmp(a, "-d") || !strcmp(a, "--debug"))
			opts.debug = 1;
		else if (!strncmp(a, "--password-file=", 16)) {
			if (!(opts.password = read_file(a + 16)))
				return 1;
		} else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
			usage(argv[0]);
			return 0;
		} else if (a[0] == '-' || opts.target) {
			usage(argv[0]);
			return 1;
		} else
			opts.target = a;
	}
	if (!opts.target) {
		usage(argv[0]);
		return 1;
	}
	return client_run(&opts);
}
