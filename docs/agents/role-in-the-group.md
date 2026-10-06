<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## ROLE IN THE GROUP

Capture source — feeds H.264/H.265 elementary streams from UVC devices into
cerastream. The standard image requires `gstreamer1.0-libuvcsrc` through its
first-party APT package list, independently of its provenance-only REPOS list.
The package provides/replaces/conflicts with `gstreamer1.0-libuvch264src` so only
one package owns the payload. HDMI capture bypasses this element.

Data flow position:
```
libuvcsrc (this) → cerastream → srtla-send-rs → srtla → irl-srt-server
```

---

