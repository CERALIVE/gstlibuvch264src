<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## VERSION SCHEME

**CalVer derivation: git tag only (no source file).**

The `.deb` version is derived **purely from git tags** at publish time via the `publish-release.yml` workflow. There is no separate `VERSION` file by design.

**Authoritative version source:** `.github/workflows/publish-release.yml` (job `calculate-version`)

**Scheme:** `YYYY.MINOR.PATCH` where:
- `YYYY` = current year (UTC)
- `MINOR` = current month (UTC, no zero-pad; e.g., `6` for June)
- `PATCH` = monotonic counter per month (incremented from git tag history)

**Example:** `2026.6.2` (June 2026, patch 2 — the hardening release)

**Tag format:** `v<VERSION>` (stable) or `v<VERSION>-beta.<N>` (beta)
- Stable: `v2026.6.2`
- Beta: `v2026.6.3-beta.1`

**Debian version:** `calculate-version` passes `VERSION` to `scripts/build-deb.sh`,
producing `gstreamer1.0-libuvcsrc_<VERSION>_<ARCH>.deb`. Old release assets stay immutable.

**No version file needed.** The workflow calculates the version at publish time from the git tag history; there is no tracked `VERSION` file in the repo. This is intentional — the single source of truth is the git tag namespace (`v*`).

---

