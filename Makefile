CC ?= cc
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
MANDIR ?= $(PREFIX)/share/man

# CFLAGS, CPPFLAGS and LDFLAGS can be replaced (ie: by the distribution's packaging flags),
# the flags the code needs are always added:
CFLAGS ?= -Os
# no build-id and no code page padding: smaller, but distribution builds want both
LDFLAGS ?= -Wl,-z,noseparate-code -Wl,--build-id=none
# size: no unwind tables (C only, no exceptions), drop unused sections.
# Hardening such as the stack protector and CET is left to the defaults: it costs nothing measurable here.
REQ_CFLAGS = -std=c99 -Wall -Wextra -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L \
	-fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections -fdata-sections
REQ_LDFLAGS = -Wl,--gc-sections
LIBS = -lX11
# extra libraries, ie: LDLIBS=-ldl for dlopen before glibc 2.34 (see the RPM spec)

CORE = src/buf.c src/rencode.c src/lz4.c src/sha256.c src/cursor.c src/pngdec.c src/webp.c
SRCS = $(CORE) src/net.c src/client.c src/x11.c src/main.c
TESTS = tests/test_core

BIN = c-xpra

$(BIN): $(SRCS) src/*.h
	$(CC) $(CPPFLAGS) $(REQ_CFLAGS) $(CFLAGS) $(REQ_LDFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LIBS) $(LDLIBS)

strip: $(BIN)
	strip -s -R .comment $(BIN)

# self-extracting compressed executable, needs upx
upx: strip
	upx -q --best --ultra-brute $(BIN)

$(TESTS): tests/test_core.c $(CORE) src/*.h
	$(CC) $(CPPFLAGS) $(REQ_CFLAGS) $(CFLAGS) -Isrc $(REQ_LDFLAGS) $(LDFLAGS) -o $@ tests/test_core.c $(CORE) $(LDLIBS)

test: $(TESTS)
	./tests/test_core

asan: CFLAGS = -O1 -g -fsanitize=address
asan: LDFLAGS = -fsanitize=address
asan: $(BIN) $(TESTS)

install: $(BIN)
	install -D -p -m 755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -D -p -m 644 packaging/$(BIN).1 $(DESTDIR)$(MANDIR)/man1/$(BIN).1

clean:
	rm -f $(BIN) $(TESTS)

.PHONY: strip upx test clean asan install
