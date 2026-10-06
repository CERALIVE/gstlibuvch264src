<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## TEST

The separate `bash tests/build-suite-contract.sh` fixture test covers matching
Trixie/Bookworm builds, mismatched suites, a non-Debian identity and missing suite
metadata (including an inherited environment value). It runs in the CI guard job.

Hardware-independent ctest suite. Two build shapes:

**Mock-backed plugin (`.so` loaded via `GST_PLUGIN_PATH`):** `test_plugin_load`, `test_mock_smoke` (+ `_asan`, `_tsan` variants). The mock plugin links the element TUs against `mock_libuvc.c` instead of real libuvc.

**Static-registration (element TUs + mock linked into one exe):** all other test targets. Mock state is in-process, so counters and config are directly readable without env vars.

```bash
# Run the full suite (with sanitizers)
cmake -B build -DENABLE_SANITIZERS=ON && cmake --build build && ctest --test-dir build --output-on-failure

# Run without sanitizers (faster)
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure

# Run a specific target
ctest --test-dir build -R "ptz_properties|ptz_capability_gate"
```

**TSan note:** `GST_OBJECT_LOCK` is a `GMutex` implemented with a raw futex in uninstrumented GLib. Under `ignore_noninstrumented_modules=1`, TSan cannot see the happens-before relationship, so it reports correctly-locked PTS/clock accesses as races. These are permanent TSan blind spots (not bugs), baselined in `tsan_pts.suppressions`. The behavioral deadlock/throughput tests (`pts_thread_safety`, `frame_throughput`) provide real regression coverage that the suppressions cannot mask.

**ASAN note:** `detect_leaks=0` is set for the mock-smoke variants (GStreamer one-time global allocs are noisy). The negotiate LSAN test uses `detect_leaks=1` with a targeted `__lsan_do_recoverable_leak_check()` after a warm-up window that swallows GStreamer's one-time globals.

**Dual-codec status [EXISTS].** Both H.264 and H.265 pad templates are present and asserted by the test suite. `cerastream` uses this element for both `InputKind::UvcH264` (negotiated to `video/x-h264`) and `InputKind::UvcH265` (negotiated to `video/x-h265`). The `libuvch26xsrc` factory alias reflects this dual-codec capability.

### Hardware-Independent Test Scope

The entire ctest suite is **mock-backed** — `tests/mock_libuvc.c` stands in for libuvc and `tests/mock_libusb.c` for libusb, so CI needs no UVC camera. This bounds what the suite can and cannot prove:

**The suite proves (in software, deterministically):**
- Element registration, pad templates, property/signal surface, and caps negotiation (`test_plugin_load`, `test_compat`, `test_functional`, `test_negotiate`).
- The pure logic that does NOT depend on a real device: the Annex-B NAL parser and its count/overflow bounds (`test_nal_parse` — including the `overflow` truncation-warning and `count_bound` suites), the SPS/PPS path builder, cache-key snapshot, and the cache file-open NULL/missing-file path (`test_cache`, `test_live_source` `spspps_key_snapshot`/`cache_open_null_path`).
- Concurrency/teardown invariants observable in-process under sanitizers: the PTS/clock lock (`test_pts_thread_safety` TSan), the SPS/PPS-bounds clamp and cache index race (ASan/TSan), USB single-`libusb_close` teardown (`test_usb_teardown`), and the CVE-2026-1991 null-guard against the vendored libuvc.
- Frame-callback-driven behavior fed by crafted access units through the mock: PTS monotonicity, IDR gating, write-on-change caching, disconnect/unlock lifecycle.
- The `alignment=au` output-buffer contract (`test_au_alignment`): a multi-slice picture aggregates into ONE buffer both with and without an AUD in the bitstream, a second picture in the same delivery starts a new buffer, and single-slice 1080p stays byte-identical to the pre-aggregation path.
- The `transfer-buffers` property contract (`test_transfer_buffers`: sentinel/clamp/reconnect re-arm, fork-only cases gated behind `TB_API_AVAILABLE` so the same test binary stays green on both `LIBUVC_USE_FORK=ON` and `OFF`) and the vid:pid quirk table (`test_quirks`: pure lookup/limits resolution, the universal bounded probe-retry policy — one `UVC_ERROR_INVALID_MODE` recovers in exactly two attempts, a second one propagates after two, `UVC_ERROR_PIPE`/`NO_DEVICE` propagate after one, and a healthy device probes exactly once — the shipped Osmo `QUIRK_MAX_PIXEL_RATE` cap, a red/green pair driving `negotiate()` against the Osmo's real advertised H.264 ladder — one case pins that an UNquirked device still picks the top mode 3840x2160@60, the other that the quirked Osmo lands on its capped ceiling 3840x2160@30 — and four `quirks_synthetic_row_*` cases that exercise every caps-filter branch (discrete-list filtering, fraction-range clamping, empty-mode drop, max-fps resolution) through an INJECTED test row, so the filter machinery stays covered independently of whatever the production table happens to hold) and the negotiation-failure descriptor inventory (`test_negotiate`'s `negotiate_inventory_logged` case).

**Hardware-only, run by hand (NOT in ctest):** `tests/board/wedge-recovery.sh` induces a real wedge on a board — a gated SIGKILL of a holder that is provably streaming, matching the kill discipline the wedge investigation used — then measures reset-to-advancing-frames through the REAL `libusb_reset_device()` path and asserts it against `reset-settle-max-ms`, with a second USB port as a negative control. `tests/board/negotiation-matrix.sh` drives real-camera negotiation, phantom-mode, and sustained drills. Its fps floor uses `(AU count - 1) / (last AU PTS - first AU PTS)` so process startup is not mistaken for sustained delivery time, while every score still reports full process wall time. Missing, malformed, or fewer than two PTS samples visibly fall back to the former wall calculation. Transition scoring requires a valid commit leg: no commit AUs are INCONCLUSIVE, an element error in the subject leg is FAIL, and only a signal-free zero-AU subject remains INCONCLUSIVE. Both hardware harnesses are deliberately absent from `tests/CMakeLists.txt`; gated board operations skip (exit 77) unless `CERALIVE_BOARD_TEST=1`.

**Hardware-free board-harness self-test:** run `bash tests/board/negotiation-matrix-selftest.sh`. Its synthetic identity logs prove a 1.3 s pre-frame startup delay changes the old wall-based verdict but not the PTS-span delivery rate, pin the explicit one-frame/malformed-PTS wall fallback, and keep transition element errors distinct from a genuinely signal-free zero-AU transcript. This test does not open a camera and is safe on a development host.

**The suite does NOT prove (requires real hardware — out of scope here):**
- Actual USB enumeration, `uvc_open()`/streaming against a physical DJI/UVC camera, real bandwidth at a given `max-payload`, or real PTZ motion on a device.
- Whether a given camera actually emits multi-slice pictures or Access Unit Delimiters. The AU-alignment cases prove the element's grouping POLICY against crafted bitstreams; which shape a real DJI/UVC device puts on the wire at 1080p30 vs 2160p30 comes only from a board capture.
- The V4L2 `VIDIOC_TRY_FMT` probe result for a real `/dev/videoN` (the test only asserts the probe is non-fatal when the node is absent).
- Mid-stream physical replug/reconnect timing (the backoff schedule is asserted via a test hook, not a real unplug).
- Real reset-to-advancing-frames timing. The mock pins the readiness POLICY (poll, reopen, require a frame, honour the bound) via the `MOCK_UVC_FRAME_SILENT` mode and the reset/poll hooks; the actual recovery duration on hardware comes only from `tests/board/wedge-recovery.sh`.

When adding tests, keep them inside the mock-coverable boundary above — assert software behavior the mock can deterministically drive, never a hardware outcome the mock cannot model. Real-hardware validation tracks separately (see `cerastream/docs/notes/hardware-validation.md` for the device-class profiles).

---

