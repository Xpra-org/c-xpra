CC ?= cc
CFLAGS ?= -Os -Wall -Wextra -std=c99 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
# size: no unwind tables (C only, no exceptions), drop unused sections, no code page padding
# (hardening such as the stack protector and CET is kept: it costs nothing measurable here)
CFLAGS += -fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections -fdata-sections
LDFLAGS ?= -Wl,--gc-sections -Wl,-z,noseparate-code -Wl,--build-id=none
LIBS = -lX11

CORE = src/buf.c src/rencode.c src/lz4.c src/sha256.c
SRCS = $(CORE) src/net.c src/client.c src/x11.c src/main.c
TESTS = tests/test_core

BIN = c-xpra

$(BIN): $(SRCS) src/*.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(SRCS) $(LIBS)

strip: $(BIN)
	strip -s -R .comment $(BIN)

# self-extracting compressed executable, needs upx
upx: strip
	upx -q --best --ultra-brute $(BIN)

$(TESTS): tests/test_core.c $(CORE) src/*.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_core.c $(CORE)

test: $(TESTS)
	./tests/test_core

asan: CFLAGS = -O1 -g -Wall -Wextra -std=c99 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L -fsanitize=address
asan: LDFLAGS =
asan: $(BIN) $(TESTS)

clean:
	rm -f $(BIN) $(TESTS)

.PHONY: strip upx test clean asan
