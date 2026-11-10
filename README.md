# c-xpra

A minimal [xpra](https://xpra.org) client in C, written clean-room from the xpra
protocol documentation only (`docs/Porting`, `docs/Network/Protocol.md`, `docs/Subsystems`).

It shows the windows of a seamless session and forwards pointer and keyboard input.
Nothing else: no audio, clipboard, notifications, cursors, tray, file transfers, ...

Dependencies: libc and libX11. LZ4 decompression, rencodeplus and HMAC-SHA256 are built in.

## Build

```shell
make          # c-xpra
make strip    # ~35KB
make upx      # ~20KB, self-extracting (needs upx)
make test     # unit tests, no display needed
make asan     # AddressSanitizer build
```

## Usage

```shell
c-xpra tcp://host[:port]        # default port 14500
c-xpra :10                      # local session, $XDG_RUNTIME_DIR/xpra/10/socket
c-xpra socket:/path/to/socket
XPRA_PASSWORD=secret c-xpra tcp://host:14500
c-xpra --password-file=FILE tcp://host:14500
c-xpra -d ...                   # log packet types
```

Without a password, the client prompts on the terminal when the server sends a challenge.
Only `hmac+sha256` authentication is implemented: use TLS / SSH tunnels on untrusted networks.

## Protocol support

| Area | Supported |
|---|---|
| Transports | TCP, Unix domain sockets |
| Records | rencodeplus, LZ4-compressed records, raw chunks (sends uncompressed) |
| Auth | `hmac+sha256` challenge / response |
| Pictures | `rgb24` / `rgb32` in any `RGB`/`BGR`/`RGBX`/`BGRX`/`RGBA`/`BGRA` order, raw or LZ4 |
| Windows | create, map, configure, move-resize, resized, metadata (title, transient-for, size-constraints), raise, close, destroy, override-redirect popups |
| Input | focus, pointer motion / buttons, `pointer-wheel` (button pairs for servers without `wheel.precise` / `wheel.emulation`), keyboard events with X11 keysym names and modifiers |
| Session | ping / ping-echo, draw acknowledgements, connection-close |

## Layout

| File | Purpose |
|---|---|
| `src/rencode.c` | rencodeplus encoder / decoder |
| `src/lz4.c` | LZ4 block decoder |
| `src/sha256.c` | SHA-256, HMAC |
| `src/net.c` | connection, record layer |
| `src/client.c` | handshake, authentication, packet handlers |
| `src/backend.h` | platform backend interface |
| `src/x11.c` | Xlib backend |

Adding a platform (GDI, Cocoa, ...) means implementing `src/backend.h` and
calling the `client_on_*` callbacks for input events.

## License

MIT, see [LICENSE](LICENSE).
