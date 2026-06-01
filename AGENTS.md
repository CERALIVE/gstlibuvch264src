# gstlibuvch264src

GStreamer source element that pulls H.264 frames directly from DJI action cameras and UVC devices via libuvc. Developed by UnlimitedIRL; forked/maintained under CeraLive.

Parent manifest: [`../AGENTS.md`](../AGENTS.md)

---

## ROLE IN THE GROUP

Capture source element — feeds raw H.264 bitstream from DJI/UVC cameras into the ceracoder pipeline. **Optional device-image component**: the image build may or may not include this plugin depending on capture hardware. HDMI capture paths bypass this element entirely.

Data flow position:
```
libuvch264src (this) → ceracoder → srtla → irl-srt-server
```

---

## STRUCTURE

```
gstlibuvch264src/
├── libuvch264src/       # GStreamer plugin source (Meson build)
│   └── src/             # C source for the element
├── libuvc/              # VENDORED — CMake library; build separately first
├── Dockerfile           # Reproducible build environment
└── README.md
```

---

## WHERE TO LOOK

| Task | Location |
|------|----------|
| Plugin element logic | `libuvch264src/src/` |
| Meson build config | `libuvch264src/meson.build` |
| libuvc vendored lib | `libuvc/` (CMake) |
| Build environment | `Dockerfile` |
| Example pipelines | `README.md` |

---

## BUILD

Two-stage build — libuvc first, then the GStreamer plugin:

```bash
# 1. Build vendored libuvc
cd libuvc && cmake . && make && sudo make install

# 2. Build plugin
meson setup build libuvch264src/
cd build && meson compile && meson install

# 3. Move .so to system GStreamer path (aarch64)
sudo mv /usr/local/lib/aarch64-linux-gnu/gstreamer-1.0/libgstlibuvch264src.so \
        /lib/aarch64-linux-gnu/gstreamer-1.0/
sudo cp /usr/local/lib/libuvc.* /usr/lib/aarch64-linux-gnu/
```

Rockchip decoder note: kernel 5.10 → `mppvideodec`; kernel 6.6 → `v4l2slh264dec`.

---

## ANTI-PATTERNS

- **DO NOT heavily modify `libuvc/`** — vendored upstream library. Patch minimally; prefer upgrading the whole vendor snapshot if fixes are needed.
- Do NOT create `libuvc/AGENTS.md` — vendored code, not a CeraLive module.
- Do NOT link against system libuvc if it exists; the vendored copy is intentional for version pinning.
- This plugin is **not** in the device image REPOS list by default — don't assume it's always present on device.
