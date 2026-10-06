# gstlibuvcsrc

Parent: [Workspace rules](https://github.com/CERALIVE/ceralive/blob/master/AGENTS.md).

<!-- workspace-hard-rules:begin -->
## Workspace hard rules (identical in every CeraLive AGENTS.md)
- Commits and PRs carry the human author only: no Co-authored-by, no AI attribution.
- Start from the updated canonical branch; rebase to update; never `reset --hard` or discard others' work.
- One focused PR per repo, opened against CERALIVE/<repo>; the root policy PR merges first.
- A repo is self-contained: no path above its root; consume @ceralive packages from the registry, never link:/file:.
- Never delete, skip or weaken a test; every behavior change ships with a test.
- A user-visible change updates docs.ceralive.tv in English and Spanish (es-419), and any ceralive.tv claim it touches, in the same release.
- AGENTS.md holds rules and routing only, within budget; contracts and history live in docs/agents/.
- Full canon: https://github.com/CERALIVE/ceralive/blob/master/AGENTS.md
<!-- workspace-hard-rules:end -->

## ROLE

Portable userspace libuvc GStreamer H.264/H.265 capture source feeding cerastream.
Requires supported camera formats and USB access; portability is not hardware qualification.

## STRUCTURE

- `libuvch264src/` — canonical Meson plugin sources and capture notes.
- `tests/` — mock-backed CMake/ctest suite and gated board drills.
- `scripts/` — pinned dependency build, packaging and CI guards.
- `patches/` — upstream fallback patches.
- `docs/` — engineering notes and preserved agent contracts.
- `.github/` — CI, release workflow and PR checklist.

## COMMANDS

```bash
bash scripts/check-source-list.sh
bash tests/build-suite-contract.sh
bash scripts/check-libuvc-fork.sh
cmake -B build -DENABLE_SANITIZERS=ON -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
cmake --build build
ctest --test-dir build --output-on-failure
bash scripts/check-reproducibility.sh
bash tests/board/negotiation-matrix-selftest.sh
```

Production: Dockerfile builds pinned libuvc, then `meson setup build ./libuvch264src/`,
`meson compile` and `meson install --no-rebuild` from `build/`.
CI builds amd64/arm64 on Trixie and Bookworm; production packaging is Trixie only:
`VERSION=0.0.0 ARCH=amd64 bash scripts/build-deb.sh`, then
`bash tests/package-contract.sh dist/gstreamer1.0-libuvcsrc_0.0.0_amd64.deb`.
See the build contract for staging and pinned dependency commands.

## WHERE TO LOOK

| Code path or task | Contract |
|---|---|
| Before changing anything else here, open docs/agents/README.md and read the contract for the subsystem you touch | [Contract index](docs/agents/README.md) |
| Overview | [overview.md](docs/agents/overview.md) |
| ROLE IN THE GROUP | [role-in-the-group.md](docs/agents/role-in-the-group.md) |
| STRUCTURE | [structure.md](docs/agents/structure.md) |
| WHERE TO LOOK | [where-to-look.md](docs/agents/where-to-look.md) |
| PROPERTIES | [properties.md](docs/agents/properties.md) |
| PTZ CONTROL SURFACE | [ptz-control-surface.md](docs/agents/ptz-control-surface.md) |
| DISCONNECT / RECONNECT BEHAVIOR | [disconnect-reconnect-behavior.md](docs/agents/disconnect-reconnect-behavior.md) |
| OUTPUT BUFFER CONTRACT (`alignment=au`) | [output-buffer-contract-alignment-au.md](docs/agents/output-buffer-contract-alignment-au.md) |
| V4L2 CAPABILITY PROBE | [v4l2-capability-probe.md](docs/agents/v4l2-capability-probe.md) |
| BUILD | [build.md](docs/agents/build.md) |
| TEST | [test.md](docs/agents/test.md) |
| VERSION SCHEME | [version-scheme.md](docs/agents/version-scheme.md) |
| ANTI-PATTERNS | [anti-patterns.md](docs/agents/anti-patterns.md) |

## HARD RULES

- Repository `gstlibuvcsrc`; element `libuvcsrc`; package `gstreamer1.0-libuvcsrc` provides/replaces/conflicts with `gstreamer1.0-libuvch264src`.
- Keep aliases `libuvch264src` and `libuvch26xsrc`, the single `libgstlibuvch264src.so`, libuvc SONAMEs, cache/socket names and persisted engine IDs.
- `libuvcsrc` is the single UVC H.264/H.265 capture path; never substitute `v4l2src` on failure.
- Probe once; retry exactly once only on `UVC_ERROR_INVALID_MODE`. No per-device probe-policy flag or production override string.
- Deep USB recovery stays default-off, privileged and bounded; escalate only after failed reset; veto hubs with sibling devices.
- Port cycling is not proven VBUS removal. No 4K60 qualification claim; raise pixel-rate caps only on advancing real hardware frames.
- Keep pinned libuvc, not system libuvc; update only the selected dependency pin with provenance and validation.
- Preserve one GstBuffer per access unit, `alignment=au`, IDR-gated parameter insertion and AUD-first ordering.
- `uvc_close()` owns the single libusb close; never call `force_usb_release()` before it.
- Recovery requires a delivered frame and context refresh; NOT_FOUND means re-enumeration; never re-arm reset on the first recovery frame.
- Keep readiness polling bounded, not a fixed settle delay; teardown can exceed the retry-loop budget.
- Control sockets stay opt-in, per-instance and private; never use a world-accessible fallback.
- Keep quirk knowledge table-driven and filtering shared by negotiate/deliverable APIs; unknown caps are NULL, not EMPTY.
- Runtime export is scratch-only plugin + three libuvc.so entries, never distro /usr; use computed multiarch paths.
- Production is suite-checked Trixie; Bookworm is portability CI. Keep package ABI/dependency ceilings and exact payload checks.
- Versions derive only from git tags; old release assets are immutable. Never add a tracked VERSION file.
- Mock tests prove software contracts, not hardware outcomes; retain sanitizer coverage and separate gated board validation.
