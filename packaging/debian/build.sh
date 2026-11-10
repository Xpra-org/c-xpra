#!/bin/bash
# Entry point for https://github.com/Xpra-org/repo-build-scripts `build_debs.sh`,
# which runs it from this directory.

set -e

eval "$(dpkg-architecture -s)"

if [ -z "${REPO_ARCH_PATH}" ]; then
	REPO_ARCH_PATH="$(pwd)/../repo"
fi

# build the version in the changelog, from the tarball `download_source.sh`
# put in ../pkgs/ (from the `Source0:` URL in c-xpra.spec):
changelog_version="$(dpkg-parsechangelog -l c-xpra/changelog -S Version)"
upstream_version="${changelog_version%-*}"
C_XPRA_TAR_GZ="../pkgs/c-xpra-${upstream_version}.tar.gz"
if [ ! -f "${C_XPRA_TAR_GZ}" ]; then
	echo "no c-xpra ${upstream_version} source found"
	exit 1
fi

source_dir="$(basename "${C_XPRA_TAR_GZ}" .tar.gz)"
rm -rf -- "./${source_dir:?}"
tar -xzf "${C_XPRA_TAR_GZ}"
pushd "./${source_dir}"
cp -a ../c-xpra debian

# skip the build if this package is already in the repository:
deb_filename="c-xpra_${changelog_version}_${DEB_BUILD_ARCH}.deb"
mkdir -p "${REPO_ARCH_PATH}"
if find "${REPO_ARCH_PATH}" -name "${deb_filename}" -print -quit | grep -q .; then
	echo "${deb_filename} already exists"
else
	mk-build-deps \
		--install \
		--remove \
		--tool='apt-get -o Debug::pkgProblemResolver=yes --no-install-recommends --yes' \
		debian/control
	debuild -us -uc -b
	find .. -maxdepth 1 -type f \
		\( -name 'c-xpra_*.deb' -o -name 'c-xpra_*.changes' -o -name 'c-xpra_*.buildinfo' \) \
		-exec cp -p {} "${REPO_ARCH_PATH}/" \;
fi

popd
