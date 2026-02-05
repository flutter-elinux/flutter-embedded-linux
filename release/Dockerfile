# Build releases with the oldest possible supported target
# The built binaries will depend on symbols from the libc used here,
# so this should work on newer OS but might not work on older OS

# Recommended version should be kept in sync with the wiki:
# https://github.com/flutter-elinux/flutter-elinux/wiki/flutter-elinux-install
FROM debian:bullseye-slim

ENV DEBIAN_FRONTEND noninteractive

RUN <<EOF
dpkg --add-architecture arm64

apt-get update
apt-get -y -q upgrade

apt-get -y install			\
	build-essential			\
	clang				\
	cmake				\
	crossbuild-essential-arm64	\
	curl				\
	equivs				\
	git				\
	libdrm-dev			\
	libdrm-dev:arm64		\
	libegl1-mesa-dev		\
	libegl1-mesa-dev:arm64		\
	libgbm-dev			\
	libgbm-dev:arm64		\
	libgles2-mesa-dev		\
	libgles2-mesa-dev:arm64		\
	libinput-dev			\
	libinput-dev:arm64		\
	libsystemd-dev			\
	libsystemd-dev:arm64		\
	libudev-dev			\
	libudev-dev:arm64		\
	libuv1-dev:arm64		\
	libwayland-dev			\
	libwayland-dev:arm64		\
	libxkbcommon-dev		\
	libxkbcommon-dev:arm64		\
	pkgconf				\
	wayland-protocols		\
	wayland-protocols:arm64		\
	xxhash				\
	xz-utils			\
	zip

# not needed for trixie
apt-get -y install libstdc++-10-dev:arm64

apt-get clean
EOF
