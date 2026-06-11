# gstlibuvch264src

GStreamer source element that pulls H.264 frames directly from DJI action cameras and UVC devices via libuvc. Developed by UnlimitedIRL; forked/maintained under CeraLive.

Parent manifest: [`../AGENTS.md`](../AGENTS.md)

---

## ROLE IN THE GROUP

Capture source element — feeds raw H.264 bitstream from DJI/UVC cameras into the cerastream pipeline. **Optional device-image component**: the image build may or may not include this plugin depending on capture hardware. HDMI capture paths bypass this element entirely.

Data flow position:
```
libuvch264src (this) → cerastream → srtla → irl-srt-server
```

---

## STRUCTURE

```
gstlibuvch264src/
├── libuvch264src/       # GStreamer plugin source (Meson build — canonical)
│   └── src/             # C source for the element
├── tests/               # gst-check plugin-load smoke test (ctest target)
├── patches/             # libuvc v0.0.7 patches (UVC 1.5 + H.265), applied at build
├── CMakeLists.txt       # TEST-ONLY build: compiles plugin + smoke test for ctest
├── Dockerfile           # Reproducible build environment (Meson)
└── README.md
```

> `libuvc/` is no longer vendored in-tree — it is cloned (upstream v0.0.7) and
> patched at build time. The Dockerfile does this for the production image; the
> top-level `CMakeLists.txt` does the same via `FetchContent` for the test build.

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

## TEST

Hardware-independent plugin-load smoke test (gst-check), wired as a ctest target
via the top-level `CMakeLists.txt`. This CMake build is **test-only** — it
compiles the plugin (and vendors libuvc via `FetchContent`) solely to run the
smoke suite. The canonical production build stays Meson (above).

```bash
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

The suite (`tests/test_plugin_load.c`) asserts: the `libuvch264src` plugin
registers; both factories (`libuvch264src` + `libuvch26xsrc` alias) exist; the
element is a `GstPushSrc`; the `index` string property defaults to `"0"`; and
the ALWAYS `src` pad template advertises `video/x-h264`. No UVC device is opened
(`gst_element_factory_make` only runs class/instance init). Runs in CI via the
`smoke-test` job in `.github/workflows/build-check.yml`.

---

## ANTI-PATTERNS

- **DO NOT heavily modify `libuvc/`** — vendored upstream library. Patch minimally; prefer upgrading the whole vendor snapshot if fixes are needed.
- Do NOT create `libuvc/AGENTS.md` — vendored code, not a CeraLive module.
- Do NOT link against system libuvc if it exists; the vendored copy is intentional for version pinning.
- This plugin is **not** in the device image REPOS list by default — don't assume it's always present on device.
