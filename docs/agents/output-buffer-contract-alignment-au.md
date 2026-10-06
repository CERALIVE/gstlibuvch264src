<!-- Moved verbatim from AGENTS.md on 2026-10-05 by lean-rules-docs-landing-latam -->

## OUTPUT BUFFER CONTRACT (`alignment=au`)

Both pad templates advertise `alignment=(string)au`, so **every `GstBuffer` the element pushes is exactly one access unit** — one displayed picture, however many NAL units the device split it into. Downstream (`h264parse`, `v4l2slh264dec`/`mppvideodec`, the muxers) trusts that claim to find frame boundaries; the element must therefore honour it rather than merely assert it.

`frame_callback()` parses one libuvc delivery into NAL units, partitions those units into access units (`split_access_units()`), and emits ONE buffer per access unit. Boundary detection, in priority order:

- **AUD present** — an Access Unit Delimiter (H.264 `nal_unit_type` 9, H.265 `AUD_NUT` 35, both mapped to `UNIT_AUD`) *is* by definition the first NAL of its access unit, so it is an exact boundary. No heuristic involved.
- **AUD absent** — the standard fallback: a slice NAL that opens a new picture ends the access unit that already holds one. "Opens a new picture" is read from the first bit of the slice payload — H.264's `first_mb_in_slice` is `ue(v)`, whose value 0 is the single bit `1`, and H.265's `first_slice_segment_in_pic_flag` is a raw `u(1)` — so a set top bit on the first payload byte means first-slice. Emulation prevention cannot disturb that byte (a `0x03` is only inserted after two `0x00` bytes, and the preceding NAL header is non-zero for every slice).
- Any non-VCL run (SEI, parameter sets) immediately preceding a new picture's first slice belongs to the **following** access unit, so the cut is placed at the head of that run.
- A device that emits neither an AUD nor a decodable first-slice bit never splits: the whole delivery becomes one access unit. That is the same grouping a single-picture delivery gets, and it is never a mid-picture cut.

**Behaviour that did NOT change:**

- Single-slice 1080p — the overwhelmingly common case — is byte-identical to the old per-NAL path: its access unit is one slice, so the one emitted buffer holds exactly the delivered bytes. Pinned by the `au_single_slice_characterization` ctest case, which was written and proven green BEFORE the aggregation landed.
- Parameter sets are still consumed from the wire and re-prepended from the cache. They are written immediately **before** the access unit's first IDR slice, never at the head of the buffer, so an AUD stays the very first NAL of its access unit.
- The pre-first-IDR gate, the SPS/PPS/VPS bounds clamp, and the write-on-change cache policy are unchanged.

**PTS convention.** An aggregated access unit carries the arrival running-time of the delivery it came from — identically the PTS its **first** slice would have been stamped with under the per-NAL path, since every NAL of one delivery shares a single arrival instant. `GST_BUFFER_OFFSET` is now an access-unit counter rather than a NAL counter; for the single-slice case the sequence is unchanged. The `prev_pts` monotonic clamp no longer fires for slices of one picture (they are aggregated); it still covers a delivery that carried more than one access unit, whose AUs share an arrival `ts`.

Regression-guarded by `tests/test_au_alignment.c` (`au_single_slice_characterization`, `au_multi_slice_aud`, `au_aud_less_fallback`).

---

