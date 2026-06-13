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
element is a `GstPushSrc`; the `index` string property defaults to `"0"`; the
ALWAYS `src` pad template advertises `video/x-h264`; **and the element also
exposes a `video/x-h265` pad template** (dual-codec confirmed — the libuvc v0.0.7
patches in `patches/` add UVC 1.5 + H.265 support). No UVC device is opened
(`gst_element_factory_make` only runs class/instance init). Runs in CI via the
`smoke-test` job in `.github/workflows/build-check.yml`.

**Dual-codec status [EXISTS].** Both H.264 and H.265 pad templates are present and
asserted by the test suite. `cerastream` uses this element for both
`InputKind::UvcH264` (negotiated to `video/x-h264`) and `InputKind::UvcH265`
(negotiated to `video/x-h265`). The `libuvch26xsrc` factory alias reflects this
dual-codec capability.

---

## VERSION SCHEME

**CalVer derivation: git tag only (no source file).**

The `.deb` version is derived **purely from git tags** at publish time via the `publish-release.yml` workflow. There is no separate `VERSION` file by design.

**Authoritative version source:** `.github/workflows/publish-release.yml` (job `calculate-version`)

**Scheme:** `YYYY.MINOR.PATCH` where:
- `YYYY` = current year (UTC)
- `MINOR` = current month (UTC, no zero-pad; e.g., `6` for June)
- `PATCH` = monotonic counter per month (incremented from git tag history)

**Example:** `2026.6.1` (June 2026, patch 1)

**Tag format:** `v<VERSION>` (stable) or `v<VERSION>-beta.<N>` (beta)
- Stable: `v2026.6.1`
- Beta: `v2026.6.2-beta.1`

**FPM .deb version:** The `VERSION` env var from `calculate-version` is passed directly to FPM's `-v` flag (line 99 in `publish-release.yml`), producing `.deb` packages with CalVer versions like `gstreamer1.0-libuvch264src_2026.6.1_arm64.deb`.

**No version file needed.** The workflow calculates the version at publish time from the git tag history; there is no tracked `VERSION` file in the repo. This is intentional — the single source of truth is the git tag namespace (`v*`).

---

## ANTI-PATTERNS

- **DO NOT heavily modify `libuvc/`** — vendored upstream library. Patch minimally; prefer upgrading the whole vendor snapshot if fixes are needed.
- Do NOT create `libuvc/AGENTS.md` — vendored code, not a CeraLive module.
- Do NOT link against system libuvc if it exists; the vendored copy is intentional for version pinning.
- This plugin is **not** in the device image REPOS list by default — don't assume it's always present on device.
