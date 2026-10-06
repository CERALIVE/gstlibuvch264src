<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## BUILD

### Production build (Meson, canonical)

```bash
# 1. Build libuvc (CeraLive fork, default) — no patch step needed
scripts/build-libuvc.sh

# To use upstream v0.0.7 + patches fallback instead:
# LIBUVC_USE_FORK=OFF scripts/build-libuvc.sh

# 2. Build plugin
meson setup build libuvch264src/
cd build && meson compile && meson install

# 3. Move .so to system GStreamer path (multiarch-aware)
MULTIARCH=$(gcc -print-multiarch)
sudo mv /usr/local/lib/${MULTIARCH}/gstreamer-1.0/libgstlibuvch264src.so \
        /lib/${MULTIARCH}/gstreamer-1.0/
sudo cp /usr/local/lib/libuvc.* /usr/lib/${MULTIARCH}/
```

`$(gcc -print-multiarch)` resolves to `aarch64-linux-gnu` on arm64, `x86_64-linux-gnu` on amd64, etc. Do not hardcode the arch string.

Downstream pairing — **variant selection, not capture-plugin compatibility**:

| Kernel / configured variant | H.264 decoder | H.265 decoder | Encoder |
|---|---|---|---|
| 5.10 with MPP drivers/userspace | `mppvideodec` | `mppvideodec` | `mpph264enc` / `mpph265enc` |
| 6.6 mainline V4L2 decode | `v4l2slh264dec` | `v4l2slh265dec` | Depends on installed encoder driver/userspace |
| 7.2 mainline | V4L2 elements available | V4L2 elements available | Depends on installed encoder driver/userspace |
| 7.2 with CeraLive island — RK3588-optimised | `mppvideodec` | `mppvideodec` | `mpph264enc` / `mpph265enc` |

MPP availability depends on the driver/UAPI and matched userspace, not a kernel
minor cutoff. CeraLive's RK3588 path pairs these elements with `rgaconvert`, its
librga fork and island drivers. Capture remains portable userspace libuvc.
Which sources the kernel exposes through UVC/V4L2 varies with kernel UVC support;
that inventory/capture-family axis is separate from downstream silicon pairing.

### Reproducible Docker build

The `Dockerfile` pins both the base image and the libuvc source:

```
debian:trixie-slim@sha256:d7e12182ce18b85b93007c1dedf31f2d29e01ccf3182cc4017c709b6259bc132
```

libuvc is fetched via `scripts/build-libuvc.sh` (fork mode by default, SHA `f3eda76` on `main`). The arch matrix fails loudly on unknown `TARGETARCH` values — no silent fallback.

**Two stages: pinned Debian 13 Trixie `build`, then `FROM scratch` `runtime`.**
The production base matches the device target suite. Bookworm remains a separate
source-portability CI build on both architectures, not the production package base.
Before installing build dependencies, `scripts/check-build-suite.sh` checks the
container's actual Debian identity and suite against `BUILD_SUITE` (default
`trixie`). CI explicitly selects `bookworm` for its portability legs; changing
only `BUILD_BASE` to the wrong suite fails before compilation. This is a build
gate, not a restriction on the portable source or its downstream decoder pairing.
`runtime` carries ONLY `usr/lib/<triplet>/gstreamer-1.0/libgstlibuvch264src.so`
and the three `libuvc.so*` entries. Never export a distro `/usr` as the payload.
`scripts/build-deb.sh` packages that tree with `dpkg-deb`, checks the GLIBC 2.41
ceiling and the libuvc ELF dependency `libjpeg.so.62`, and declares the Trixie
runtime dependencies.
`tests/package-contract.sh` checks the exact payload and old-name compatibility.
`GST_PLUGIN_DEFINE` names CeraLive and this repository as package/origin: this is
a diagnostic metadata judgement, with no change to registration or media behavior.

---

