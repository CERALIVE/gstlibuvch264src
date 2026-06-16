# Multi-architecture support (driven by buildx --platform; CI builds amd64 + arm64)
ARG TARGETARCH

# Base pinned to a digest (not the mutable :latest / floating :24.04 tag) so the
# build is reproducible and verifiable offline. Refresh via:
#   docker buildx imagetools inspect ubuntu:24.04   (use the index Digest)
FROM ubuntu:24.04@sha256:786a8b558f7be160c6c8c4a54f9a57274f3b4fb1491cf65146521ae77ff1dc54 AS build

# Set the working directory inside the container
WORKDIR /app

# Install essential build tools, GStreamer dependencies, and libusb
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y \
	build-essential \
	cmake \
	git \
	meson \
	ninja-build \
	patch \
	pkg-config \
	libgstreamer1.0-dev \
	libgstreamer-plugins-base1.0-dev \
	libjpeg-dev \
	libusb-1.0-0 \
	libusb-1.0-0-dev

# Copy plugin sources and patches into the image
COPY . /app

# Build and install libuvc via the single-source build script
# (scripts/build-libuvc.sh) so the pinned SHAs, the CMake options, and the
# fallback patch steps live in exactly ONE place, shared verbatim with the test
# build in CMakeLists.txt. The plugin needs UVC_FRAME_FORMAT_H265 (absent from
# stock libuvc), UVC 1.5 header parsing, and the libusb auto-detach call needed
# to claim devices bound to the uvcvideo kernel driver — all carried by the fork.
#
# Source selected by the LIBUVC_USE_FORK build arg (see the ADR at
# libuvch264src/docs/notes/libuvc-fork-adr.md):
#
#   1 (default): CeraLive/libuvc fork at the pinned ceralive-v0.0.7.1 SHA. The
#                three changes are commits on the fork, so NO patch(1) step runs.
#   0 (rollback): upstream v0.0.7 at its pinned SHA + the UVC 1.5 / H.265 patches
#                from patches/ (the pre-fork path). Build with
#                `docker build --build-arg LIBUVC_USE_FORK=0 ...` to use it.
#
# The script owns the SHAs, the git-init/fetch/checkout idiom, the patch steps,
# and the CMake options; here it installs into /usr/local and runs ldconfig.
WORKDIR /app
ARG LIBUVC_USE_FORK=1
RUN if [ "${LIBUVC_USE_FORK}" = "1" ]; then _mode=fork; else _mode=upstream; fi && \
	bash scripts/build-libuvc.sh \
		--mode="${_mode}" \
		--prefix=/usr/local \
		--src-dir=/app/libuvc-build \
		--ldconfig

# Build and install libuvch264src
WORKDIR /app
RUN meson setup build ./libuvch264src/
WORKDIR /app/build
RUN meson compile
RUN meson install --no-rebuild

# Stage the runtime payload — only the plugin .so + libuvc.so* — under /out,
# mirroring the on-device /usr layout. cp -a preserves the libuvc.so symlink
# chain; the libuvc.a/.pc are build-only and excluded. TARGETARCH is a
# buildx-predefined arg, re-declared here so the RUN can read it.
ARG TARGETARCH
RUN GNUARCH=$(case "${TARGETARCH}" in \
	"arm64") echo "aarch64-linux-gnu" ;; \
	"amd64") echo "x86_64-linux-gnu" ;; \
	*) echo "Unsupported architecture: '${TARGETARCH}' (expected arm64 or amd64)" >&2; exit 1 ;; \
	esac) && \
	mkdir -p /out/usr/lib/${GNUARCH}/gstreamer-1.0 && \
	cp -a /usr/local/lib/*/gstreamer-1.0/libgstlibuvch264src.so /out/usr/lib/${GNUARCH}/gstreamer-1.0/ && \
	cp -a /usr/local/lib/libuvc.so* /out/usr/lib/${GNUARCH}/

# Runtime stage MUST stay `FROM scratch`. The release workflow exports this final
# stage wholesale (`buildx --output type=local,dest=build` → `fpm build/usr/=/usr/`).
# An ubuntu stage here exported the entire distro /usr, producing a ~56 MB .deb
# that dpkg-file-conflicts with coreutils/libc on install. GStreamer/libusb/libjpeg
# are runtime deps from the target system (see package Depends), not bundled.
FROM scratch AS runtime
COPY --from=build /out/ /
