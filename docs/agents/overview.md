<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

# gstlibuvcsrc

Portable userspace libuvc GStreamer source for UVC H.264 **and** H.265. Capture
works independently of kernel version and platform; it is not RK3588-bound.
It requires GStreamer/core and base ≥1.14, libuvc and libusb, USB access and a
device exposing supported formats. No claim that every camera/mode is qualified.
Developed by UnlimitedIRL; forked/maintained under CeraLive.

> **Security:** CVE-2026-1991 (null-deref in scan-streaming path) is fixed in the CeraLive fork at commit `eae7f49` (first shipped in tag `ceralive-v0.0.7.2`, carried forward in `ceralive-v0.0.7.9`, SHA `ada082b5009e38a89eb7cd6176683b508cd99ff5`) and also carried as `patches/cve-2026-1991-scan-streaming-nullguard.patch` for the upstream fallback path. Upstream libuvc is effectively dead (last commit 2024); the CeraLive fork at `https://github.com/CeraLive/libuvc.git` is the canonical dependency.

Canonical factory: `libuvcsrc`. `libuvch264src` and `libuvch26xsrc` are retained
aliases of the same GType. Internal source paths, plugin identity and the single
`libgstlibuvch264src.so`, libuvc SONAMEs, cache keys and socket paths stay stable.

---

