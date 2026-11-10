# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

`c-xpra` is a minimal xpra client in C99. It shows the windows of an xpra session and forwards pointer and keyboard input, and nothing else. Its only dependencies are libc and libX11. The goal is a very small, simple executable (~36 KB stripped, ~20 KB with UPX).

**Clean-room rule:** implement protocol behaviour ONLY from the xpra documentation in `~/projects/xpra/docs/`. Never read or copy the xpra source code. The most useful pages are:
- `Porting/Minimal-Client.md`, `Porting/Handshake.md`, `Porting/Authentication.md` (with test vectors), `Porting/Transports.md`
- `Network/Protocol.md`: record layer (§3), rencodeplus (§4.2), packet registry (§7–8)
- `Subsystems/{Window,Encoding,Pointer,Keyboard,Display}.md`

When the docs are ambiguous, resolve the question by black-box testing against a real `xpra` server (see below), and tell the user so the docs can be clarified. That is how we settled that the `pointer-wheel` `distance` is +1000 per click in the button's own direction.

## Commands

```shell
make            # build ./c-xpra
make strip      # stripped release binary
make upx        # UPX-compressed binary (optional, needs upx)
make test       # build and run tests/test_core (no display needed)
make asan       # AddressSanitizer build of the client and the tests (libubsan is not installed here)
make install    # PREFIX=/usr/local, DESTDIR supported; also installs packaging/c-xpra.1
make clean
./c-xpra -d tcp://127.0.0.1:14500   # -d logs each packet type sent (>) and received (<)
```

`tests/test_core.c` is a single binary with no test filter: every `test_*` function is called from `main()`. To run just one, comment out the other calls in `main()`. Run `make clean` after `make asan`, otherwise the next normal build may reuse the ASan objects and flags.

Keep the build free of warnings (`-Wall -Wextra`). In the Makefile, `REQ_CFLAGS`/`REQ_LDFLAGS` are the flags the code needs and are always added. `CFLAGS`/`LDFLAGS` only hold the size-oriented defaults (`-Os`, no build-id, no code page padding), which distribution builds replace with their own: keep anything the code depends on out of them. The stack protector and CET hardening are deliberately left on: removing them saved no bytes.

## Packaging

`packaging/` holds the RPM spec and the Debian files for [repo-build-scripts](https://github.com/Xpra-org/repo-build-scripts). It follows the layout of the sibling `~/projects/go-xpra/packaging` and `~/projects/rust-xpra/packaging`. `packaging/README.md` has the details, including a local `rpmbuild` recipe.
- **Versions:** a version bump touches `rpm/c-xpra.spec` (`Version`, `%changelog`), `debian/c-xpra/changelog`, the version in `packaging/c-xpra.1`, and the `build.version` string in `send_hello` in `src/client.c`.
- **Release checksum:** each release needs the spec's `source_sha256` set to the release tarball's checksum.
- **Man page:** keep `packaging/c-xpra.1` in sync with the command-line options in `src/main.c`.

## Integration testing

Use a private Xvfb as the client display and a throwaway xpra server:

```shell
Xvfb :99 -screen 0 1280x1024x24 &
xpra start :117 --start=xterm --bind-tcp=127.0.0.1:14517 --daemon=yes --mdns=no --pulseaudio=no --notifications=no --systemd-run=no
DISPLAY=:99 ./c-xpra -d :117 &            # or tcp://127.0.0.1:14517
DISPLAY=:99 xdotool search --name . ...   # drive input
DISPLAY=:99 xwd -root -silent | magick xwd:- shot.png   # inspect the result
```

- **Auth:** add `,auth=password,value=secret` to `--bind-tcp` and pass `XPRA_PASSWORD`.
- **Window manager:** run `metacity --replace` on `:99` to test reparenting, decorations and the close button.
- **Cleanup:** stop only what you started. Use `xpra stop :117` and kill the Xvfb by its PID. Never use `pkill -f '<pattern>'`, because it matches the invoking shell's own command line. Never use `pkill -x Xvfb`, because it can kill other users' displays.

## Architecture

The client is built in layers, from the bytes on the wire up to the native windows:

1. **`rencode.c`**: rencodeplus decoder and encoder.
   - Decoded `rv` strings and bytes are **zero-copy**: they point into the record payload or the raw-chunk buffers. They are valid only until `packet_free()`, so copy anything a handler needs to keep.
   - `enc_list(n)` and `enc_dict(n)` only emit the fixed-size forms (list < 64 items, dict < 25 pairs) and `abort()` beyond that. Use `enc_list_var` / `enc_dict_var` + `enc_end` for larger or uncounted containers. The hello uses the variable form.
   - `rv_lookup("a.b")` follows the doc's capability rules: it checks for the literal flat key first, then the nested maps, and returns a map's `""` entry when the value is itself a map.
2. **`net.c`**: transport and record layer.
   - Accepts these targets: `tcp://`, `socket:`, a bare path, `:N`, or `host:port`. `:N` resolves to `$XDG_RUNTIME_DIR/xpra/N/socket`.
   - Validates each 8-byte header and decompresses LZ4 records.
   - Raw chunks (at most 3) are buffered and substituted into the main packet's placeholder slots.
   - Outgoing records are always rencodeplus and uncompressed. This is allowed because the hello advertises the `none` compressor.
3. **`client.c`**: protocol state in a single global struct `C`.
   - The `HANDLERS[]` table maps packet names to handlers. Unknown packets are silently ignored.
   - Packets marked `needs_hello` are rejected before the server's hello arrives.
   - The client also implements the backend → client input callbacks (`client_on_*`).
4. **`backend.h`**: the platform interface. `x11.c` is the only implementation today; GDI and Cocoa backends are expected later.
   - Backends own the native windows and a backing store of native-endian `0x00RRGGBB` pixels.
   - They report input through the `client_on_*` callbacks declared in `backend.h`, using window-relative coordinates. The one exception is `client_on_configure`, which takes absolute coordinates.
   - The `MOD_*` bits follow the X11 modifier masks (shift, lock, control, mod1..mod5), so `x11.c` passes `state & 0xff` straight through.
   - A new backend should not need any changes to `client.c`.

## Protocol invariants

Keep these true when changing anything:

- **Coordinates:** everything is in server pixels. The hello sets `monitor-relative: false`, so positions are absolute. Pointer positions are the window's tracked server `x,y` plus the local event position.
- **Configure echo:** `client_on_configure` compares the new geometry against the tracked geometry. This stops the client from echoing back moves and resizes the server itself requested.
- **Acknowledgements:** every `window-draw` MUST get exactly one `window-ack`, including on failure (`-1` = decode error, `-2` = unknown window). Present the window only when `flush` is absent or 0.
- **Advertised features:** the client advertises only `rgb` (`rgb24`/`rgb32`, raw or `rgb_lz4`), `hmac+sha256` authentication and the `rencodeplus` encoder. Adding an encoding or digest means implementing it, adding it to `send_hello`, and adding a unit test.
- **Wheel:** use `pointer-wheel` only when the server's hello has `wheel.precise` or `wheel.emulation`. Otherwise send wheel buttons 4–7 as `pointer-button` press/release pairs.
- **Disconnects:** the process exits 0 only for the "normal" `connection-close` reasons listed in Protocol.md §5.3, and 1 for anything else.
