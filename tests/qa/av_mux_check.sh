#!/usr/bin/env bash
#
# mpegtsmux jitter-tolerance QA (harden-v2 Task 14) - hardware-independent.
#
# Validates the Metis top-risk: raw post-Option-B jitter into mpegtsmux. The
# mock-backed libuvch264src element (no camera) is driven through the real
# cerastream muxer topology:
#
#   libuvch264src ! h264parse ! queue ! mpegtsmux. ;
#   audiotestsrc is-live=true ! audioconvert ! <aac|mp2> ! <parser> ! queue ! mpegtsmux. ;
#   mpegtsmux ! filesink
#
# The element stamps PTS=DTS=running-time at the instant each frame arrives
# (Option B, Task 5). Under a brisk live feed the OS scheduling / processing
# jitter on those arrival instants IS the synthetic arrival jitter that reaches
# mpegtsmux - the exact production condition under test. The output .ts is then
# probed with `ffprobe -show_packets`; the run FAILS if ffprobe reports a "non
# monotonous DTS" (or any DTS/PTS regression per stream).
#
# Audio leg: audiotestsrc (NEVER a real device / alsasrc) + an MPEG-TS-legal
# audio codec. AAC (voaacenc -> avenc_aac -> faac, framed by aacparse) is
# preferred to match cerastream's SRT path; if no AAC encoder is installed the
# script falls back to MPEG-1 audio (twolamemp2enc -> lamemp3enc, framed by
# mpegaudioparse), which is the canonical MPEG-TS audio codec. Opus is never used
# with mpegtsmux (Opus-in-TS is non-standard).
#
# Usage:
#   av_mux_check.sh [av|audio]
#     av     full A/V mux (default)
#     audio  audio-only into TS (isolates the audio DTS timeline)
#
# Env overrides:
#   MOCK_PLUGIN_DIR    dir holding the mock libgstlibuvch264src.so
#                      (default: first build*/gstreamer-1.0-mock found)
#   NUM_FRAMES         video frames the mock feeds (default 120)
#   FRAME_INTERVAL_US  mock feeder cadence in us (default 8000 ~= 125 fps, brisk)
#   WORKDIR            scratch dir (default: a mktemp dir, removed on exit)
#
# Exit 0 = PASS, non-zero = FAIL or unmet prerequisite.

set -uo pipefail

MODE="${1:-av}"
NUM_FRAMES="${NUM_FRAMES:-120}"
FRAME_INTERVAL_US="${FRAME_INTERVAL_US:-8000}"

say()  { printf '%s\n' "$*"; }
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

# --- scratch dir (isolated GStreamer registry + HOME live here) ---------------
CLEAN_WORKDIR=0
if [ -n "${WORKDIR:-}" ]; then
  mkdir -p "$WORKDIR"
else
  WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/av_mux_check.XXXXXX")" || fail "mktemp failed"
  CLEAN_WORKDIR=1
fi
cleanup() { [ "$CLEAN_WORKDIR" = 1 ] && rm -rf "$WORKDIR"; }
trap cleanup EXIT

say "=== mpegtsmux jitter-tolerance QA (mode=$MODE) ==="
say "workdir: $WORKDIR"

# --- 1. required tools --------------------------------------------------------
command -v gst-launch-1.0 >/dev/null 2>&1 || fail "gst-launch-1.0 not found"
command -v gst-inspect-1.0 >/dev/null 2>&1 || fail "gst-inspect-1.0 not found"
command -v ffprobe >/dev/null 2>&1 || fail "ffprobe not found (install ffmpeg)"

have() { gst-inspect-1.0 "$1" >/dev/null 2>&1; }

for el in mpegtsmux h264parse audiotestsrc audioconvert queue filesink; do
  have "$el" || fail "required GStreamer element '$el' missing"
done

# --- 2. locate the mock-backed plugin ----------------------------------------
if [ -z "${MOCK_PLUGIN_DIR:-}" ]; then
  SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
  MOCK_SO=""
  # Prefer the canonical build dir (AGENTS.md); only it is guaranteed in sync
  # with the current tree. Other build*/ dirs (e.g. an audit build) may carry a
  # stale mock without the latest changes, so never auto-pick them by name.
  if [ -f "$REPO_ROOT/build/gstreamer-1.0-mock/libgstlibuvch264src.so" ]; then
    MOCK_SO="$REPO_ROOT/build/gstreamer-1.0-mock/libgstlibuvch264src.so"
  else
    # Fall back to the most recently built match so a freshly compiled plugin
    # wins over any older sibling build dir.
    MOCK_SO="$(find "$REPO_ROOT" -path '*/gstreamer-1.0-mock/libgstlibuvch264src.so' \
               -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -n1 | cut -d' ' -f2-)"
  fi
  [ -n "$MOCK_SO" ] || fail "mock plugin not found; build it (cmake --build build --target gstlibuvch264src_mock) or set MOCK_PLUGIN_DIR"
  MOCK_PLUGIN_DIR="$(dirname "$MOCK_SO")"
fi
[ -f "$MOCK_PLUGIN_DIR/libgstlibuvch264src.so" ] || fail "no libgstlibuvch264src.so in MOCK_PLUGIN_DIR=$MOCK_PLUGIN_DIR"
say "mock plugin: $MOCK_PLUGIN_DIR/libgstlibuvch264src.so"

# Isolate the registry/HOME but KEEP the system plugin path so mpegtsmux,
# h264parse, x264enc, voaacenc, audiotestsrc (system plugins) stay visible; the
# mock is layered on top via GST_PLUGIN_PATH.
export GST_PLUGIN_PATH="$MOCK_PLUGIN_DIR"
export GST_REGISTRY="$WORKDIR/registry.bin"
export GST_REGISTRY_FORK=no
export HOME="$WORKDIR/home"; mkdir -p "$HOME"

have libuvch264src || fail "mock element 'libuvch264src' not loadable from $MOCK_PLUGIN_DIR"

# --- 3. choose an MPEG-TS-legal audio encoder + parser ------------------------
# AAC preferred (cerastream SRT parity); MPEG-1 audio is the canonical TS
# fallback. Each codec needs its framing parser before mpegtsmux: AAC -> aacparse
# (framed), MPEG-1 -> mpegaudioparse (parsed). Opus is intentionally excluded.
AENC=""; APARSE=""; ACODEC=""
if   have voaacenc   && have aacparse;       then AENC=voaacenc;      APARSE=aacparse;       ACODEC="AAC (voaacenc)"
elif have avenc_aac  && have aacparse;       then AENC=avenc_aac;     APARSE=aacparse;       ACODEC="AAC (avenc_aac)"
elif have faac       && have aacparse;       then AENC=faac;          APARSE=aacparse;       ACODEC="AAC (faac)"
elif have twolamemp2enc && have mpegaudioparse; then AENC=twolamemp2enc; APARSE=mpegaudioparse; ACODEC="MP2 (twolamemp2enc, AAC unavailable)"
elif have lamemp3enc && have mpegaudioparse; then AENC=lamemp3enc;    APARSE=mpegaudioparse; ACODEC="MP3 (lamemp3enc, AAC unavailable)"
else
  fail "no MPEG-TS-legal audio encoder+parser available (need one of voaacenc/avenc_aac/faac+aacparse or twolamemp2enc/lamemp3enc+mpegaudioparse)"
fi
say "audio codec: $ACODEC ; parser: $APARSE"

# --- 4. generate a real H.264 access unit for the mock to replay --------------
# The mock's crafted NALs are zero-filled and h264parse rejects them as a broken
# bit stream, so a real encoder-produced IDR (SPS+PPS+IDR) is fed to the element
# via MOCK_UVC_ES_FILE. key-int-max=1 + config-interval=-1 keeps SPS/PPS in band.
have x264enc || fail "x264enc not available to synthesize a reference H.264 frame"
REF_ES="$WORKDIR/ref-frame.h264"
gst-launch-1.0 -q videotestsrc num-buffers=1 \
  ! video/x-raw,width=320,height=240,framerate=30/1 \
  ! x264enc key-int-max=1 tune=zerolatency \
  ! h264parse config-interval=-1 \
  ! video/x-h264,stream-format=byte-stream,alignment=au \
  ! filesink location="$REF_ES" >/dev/null 2>"$WORKDIR/ref.err" \
  || { cat "$WORKDIR/ref.err" >&2; fail "reference H.264 frame generation failed"; }
[ -s "$REF_ES" ] || fail "reference H.264 frame is empty"
say "reference AU: $(stat -c%s "$REF_ES") bytes"

# --- 5. run the mux pipeline --------------------------------------------------
TS="$WORKDIR/out.ts"
GST_OUT="$WORKDIR/gst.out"
GST_ERR="$WORKDIR/gst.err"

if [ "$MODE" = "audio" ]; then
  say "pipeline: audiotestsrc(is-live) ! audioconvert ! $AENC ! $APARSE ! mpegtsmux ! filesink"
  MOCK_UVC_ES_FILE="$REF_ES" timeout 60 gst-launch-1.0 -e \
    audiotestsrc is-live=true num-buffers="$NUM_FRAMES" \
      ! audioconvert ! "$AENC" ! "$APARSE" ! queue ! mpegtsmux ! filesink location="$TS" \
    >"$GST_OUT" 2>"$GST_ERR"
  GST_RC=$?
else
  say "pipeline: [mock]libuvch264src ! h264parse ! queue ! mux. + audiotestsrc ! $AENC ! $APARSE ! queue ! mux. ; mux ! filesink"
  MOCK_UVC_ES_FILE="$REF_ES" MOCK_UVC_FRAME_INTERVAL_US="$FRAME_INTERVAL_US" timeout 60 gst-launch-1.0 -e \
    libuvch264src num-buffers="$NUM_FRAMES" ! h264parse ! queue \
      ! mpegtsmux name=mux ! filesink location="$TS" \
    audiotestsrc is-live=true num-buffers="$NUM_FRAMES" \
      ! audioconvert ! "$AENC" ! "$APARSE" ! queue ! mux. \
    >"$GST_OUT" 2>"$GST_ERR"
  GST_RC=$?
fi

say "--- gst-launch exit: $GST_RC ---"
[ "$GST_RC" -eq 0 ] || { sed -n '1,40p' "$GST_ERR" >&2; sed -n '1,40p' "$GST_OUT" >&2; fail "mux pipeline did not reach EOS cleanly"; }
[ -s "$TS" ] || fail "output .ts is empty"
say "output .ts: $(stat -c%s "$TS") bytes"

# gst-launch prints bus WARNING/ERROR to stdout; scan both streams.
if grep -qi "Broken bit stream" "$GST_OUT" "$GST_ERR"; then
  grep -i "Broken bit stream" "$GST_OUT" "$GST_ERR" | head -3 >&2
  fail "h264parse reported a broken bit stream"
fi
if grep -qiE "ERROR" "$GST_OUT" "$GST_ERR"; then
  grep -iE "ERROR" "$GST_OUT" "$GST_ERR" | head -5 >&2
  fail "pipeline reported an ERROR"
fi

# --- 6. ffprobe: stream inventory + DTS-ordering asserts ----------------------
FF_ERR="$WORKDIR/ffprobe.err"
say "--- ffprobe streams ---"
ffprobe -v error -show_entries stream=index,codec_type,codec_name -of csv "$TS" 2>>"$FF_ERR" || true

# `ffprobe -show_packets` surfaces the libavformat "non monotonous DTS" demux
# warning on stderr if the muxer emitted out-of-order DTS.
PKTS="$WORKDIR/packets.csv"
ffprobe -v error -show_entries packet=stream_index,dts,pts -of csv=p=0 "$TS" \
  >"$PKTS" 2>"$FF_ERR"

if grep -qiE "non.?monoton" "$FF_ERR"; then
  grep -iE "non.?monoton" "$FF_ERR" | head -5 >&2
  fail "ffprobe reported non-monotonous DTS"
fi
[ -s "$PKTS" ] || fail "ffprobe produced no packets"

# Per-stream DTS and PTS must be non-decreasing. awk returns the assertion verdict.
ASSERT="$(awk -F, '
  { s=$1; dts=$2; pts=$3 }
  dts != "N/A" {
    if (s in ld && dts+0 < ld[s]+0) { dbad[s]++; }
    ld[s]=dts; dc[s]++
  }
  pts != "N/A" {
    if (s in lp && pts+0 < lp[s]+0) { pbad[s]++; }
    lp[s]=pts; pc[s]++
  }
  END {
    fail=0
    for (s in dc) {
      printf "stream %s: %d DTS pkts, %d DTS regressions; %d PTS pkts, %d PTS regressions\n",
             s, dc[s], (s in dbad?dbad[s]:0), (s in pc?pc[s]:0), (s in pbad?pbad[s]:0)
      if ((s in dbad) || (s in pbad)) fail=1
    }
    if (length(dc) == 0) { print "no packets carried a DTS"; fail=1 }
    print (fail ? "VERDICT_FAIL" : "VERDICT_OK")
  }' "$PKTS")"
say "$ASSERT"

# Stream-inventory expectations: A/V mode needs both video and audio; audio mode
# needs an audio stream.
TYPES="$(ffprobe -v error -show_entries stream=codec_type -of csv=p=0 "$TS" 2>/dev/null | tr -d '\r')"
if [ "$MODE" = "av" ]; then
  printf '%s\n' "$TYPES" | grep -q '^video$' || fail "no video stream in output .ts"
fi
printf '%s\n' "$TYPES" | grep -q '^audio$' || fail "no audio stream in output .ts"

printf '%s\n' "$ASSERT" | grep -q "VERDICT_OK" || fail "per-stream DTS/PTS not monotonic"

say "=== PASS: monotonic DTS/PTS, no DTS-ordering errors (mode=$MODE) ==="
exit 0
