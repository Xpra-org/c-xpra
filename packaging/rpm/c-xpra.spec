# This file is part of c-xpra.
# Copyright (C) 2026 Antoine Martin <antoine@xpra.org>
# c-xpra is released under the terms of the MIT license, see the file LICENSE for details.

# Let rpmbuild download the tarball from the `Source0:` URL below when it is not already
# in `SOURCES` (rpm refuses to fetch remote sources by default). What makes that safe is
# the sha256 check at the top of `%%prep`: the archive is only unpacked if it matches.
%define _disable_source_fetch 0

# The sha256 of the release tarball: this has to be updated for every new version.
# It can be overridden for local test builds: rpmbuild --define "source_sha256 ..."
%{!?source_sha256:%global source_sha256 e8c2618132edc9d4f90d0345e961f5ab3caeb65a956b6c077e85c1b4e7da32e4}

Name:				c-xpra
Version:			0.1.1
Release:			1%{?dist}
Summary:			Minimal Xpra client written in C
License:			MIT
URL:				https://github.com/Xpra-org/c-xpra
Packager:			Antoine Martin <antoine@xpra.org>
Vendor:				https://xpra.org/
# the "Source code (tar.gz)" of https://github.com/Xpra-org/c-xpra/releases/tag/v0.1.1;
# the `#/` fragment renames the github archive to something less ambiguous than `v0.1.1.tar.gz`
Source0:			%{url}/archive/refs/tags/v%{version}.tar.gz#/%{name}-%{version}.tar.gz

BuildRequires:		gcc
BuildRequires:		make
BuildRequires:		pkgconfig(x11)

%if 0%{?el8}
# glibc 2.28: dlopen and dlsym are still in libdl
%global make_libs LDLIBS=-ldl
%endif

%description
c-xpra is a minimal Xpra client: it connects to an Xpra server over TCP or a
local socket and shows the windows of a seamless session on the local X11
display, forwarding pointer and keyboard input and ringing the bell. It has no other dependency
than libX11, and requires an Xpra 8.0 or later server.


%prep
# the tarball may have just been downloaded by `_disable_source_fetch` above,
# so verify it before unpacking anything:
sha256=`sha256sum %{SOURCE0} | awk '{print $1}'`
if [ "${sha256}" != "%{source_sha256}" ]; then
	echo "invalid checksum for %{SOURCE0}: ${sha256}"
	exit 1
fi
%autosetup


%build
# the Makefile adds the flags the code needs to the distribution's CFLAGS and LDFLAGS
%set_build_flags
%make_build %{?make_libs}


%check
# each rpm section runs in its own shell, hence the repeated environment:
%set_build_flags
make test %{?make_libs}


%install
%make_install PREFIX=%{_prefix} %{?make_libs}


%files
%license LICENSE
%doc README.md
%{_bindir}/%{name}
# the glob matches whatever compression `brp-compress` applied (.gz, or .zst on newer Fedora)
%{_mandir}/man1/%{name}.1*


%changelog
* Tue Nov 10 2026 Antoine Martin <antoine@xpra.org> 0.1.1-1
- Features:
   png and webp pictures, when libpng / libwebp are installed
   window icons, when libpng is installed
   application cursors, in colour when libXcursor is installed
   bell forwarding
   server events and notifications printed on stdout
- Fixes:
   hello: send client_type, the client version and the double-click distance as an x, y pair

* Tue Nov 10 2026 Antoine Martin <antoine@xpra.org> 0.1.0-1
- Platforms, build and packaging:
   add RPM and Debian packaging, and the manual page
   honour the distribution's compiler and linker flags
   add an install target
- Network:
   TCP and Unix domain socket transports
   rencodeplus packets, LZ4 compressed records and raw chunks
   hmac+sha256 authentication
- Features:
   X11 backend: window forwarding with rgb24 / rgb32 pictures
   override-redirect popups, transient windows and size constraints
   pointer, wheel and keyboard input
