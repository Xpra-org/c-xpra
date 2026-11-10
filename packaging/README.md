# Packaging

Build definitions for [Xpra-org/repo-build-scripts](https://github.com/Xpra-org/repo-build-scripts),
which builds RPM and DEB packages in per-distribution containers. A single package is
produced: `c-xpra`.

```
packaging/
├── target-repository        the repository the packages are destined for (beta/stable/lts)
├── c-xpra.1                 man page, installed by `make install`
├── rpm/
│   ├── default.list         the build manifest: one entry, `c-xpra`
│   └── c-xpra.spec
└── debian/
    ├── build.sh             unpacks the tarball and runs debuild
    └── c-xpra/              becomes the source tree's `debian/` directory
        ├── changelog
        ├── control
        ├── copyright
        ├── rules
        └── c-xpra.docs
```

## Building

```shell
git clone https://github.com/Xpra-org/repo-build-scripts
cd repo-build-scripts
./setup_build_containers.sh
ln -sf /path/to/c-xpra/packaging .
./build_all.sh
```

`download_source.sh` (called by `build_all.sh`) fetches the tarball named by the
`Source0:` URL in `rpm/c-xpra.spec` into `pkgs/`. Both the RPM and the DEB build
consume that same tarball, so the version there and in `debian/c-xpra/changelog` must
match a pushed `v<version>` git tag. The spec sets `%define _disable_source_fetch 0`,
so `rpmbuild` downloads the tarball by itself when it is not already in `SOURCES`, and
`%prep` refuses to unpack anything whose sha256 does not match `source_sha256`.

**Releasing a version means updating that checksum**, which is the `sha256sum` of the
"Source code (tar.gz)" of the GitHub release. Until the first release is tagged it is a
placeholder, and builds fail in `%prep` on purpose.

A local RPM test build can override it:

```shell
mkdir -p ~/rpmbuild/SOURCES
tar --transform 's,^\.,c-xpra-0.1.0,' --exclude=./.git -czf ~/rpmbuild/SOURCES/c-xpra-0.1.0.tar.gz .
rpmbuild --define "source_sha256 $(sha256sum ~/rpmbuild/SOURCES/c-xpra-0.1.0.tar.gz | cut -d' ' -f1)" \
    -ba packaging/rpm/c-xpra.spec
```

## Notes

* **The packages use the distribution's compiler and linker flags.** The Makefile only
  adds the flags the code needs (`-std=c99`, the feature test macros, section garbage
  collection) to whatever `CFLAGS`, `CPPFLAGS` and `LDFLAGS` it is given, so the RPM and
  DEB binaries are built with the distribution's hardening and optimization, and with a
  build-id for the debuginfo packages. The size-oriented defaults (`-Os`, no build-id, no
  code page padding) only apply to plain `make` builds.
* **The binary is installed as `c-xpra`**, not `xpra`: `/usr/bin/xpra` belongs to the
  python `xpra` package, and the two can be installed side by side.
* **There is no desktop entry.** `c-xpra` has no connection dialog and needs a target
  argument, so a menu launcher would only print its usage.
* The only runtime dependency is libX11, found by the automatic dependency generators
  (`dh_shlibdeps`, rpm's ELF dependency generator). libXcursor (colour cursors), libpng
  (`png` pictures and window icons) and libwebp (`webp` pictures) are optional and loaded with `dlopen`, so the generators do not see them.
