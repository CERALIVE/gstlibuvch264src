#!/usr/bin/env bash
#
# Wedged-device recovery, on real hardware.
#
# NOT part of the ctest suite and deliberately NOT registered in
# tests/CMakeLists.txt: it needs a physical UVC camera, root, and a USB port it
# is allowed to reset, none of which exist in CI. Run it by hand on a board.
#
# What it proves that the mock suite cannot: that the readiness-based recovery
# drives the REAL libusb_reset_device() path against a REAL device, and how long
# that actually takes. The mock pins the policy's logic; only this script can
# produce the reset-to-advancing-frames number.
#
# Two fault modes, because they answer different questions:
#
#   --fault sigkill    Kill a holder that is provably streaming and see whether
#                      the device is left wedged for the next opener. This is
#                      the historical trigger. A cycle where the device comes
#                      back healthy is reported NO-WEDGE and excluded from the
#                      timings - the recovery never ran, so scoring it either
#                      way would be dishonest.
#                      --kill-mode cgroup (default) also kills libuvc's reattach
#                      helper, which double-forks and setsid()s but stays in the
#                      creator's cgroup - i.e. exactly what systemd's
#                      KillMode=mixed + FinalKillSignal=9 teardown reaches.
#                      --kill-mode holder kills only the holder, letting the
#                      helper win and repair the binding.
#
#   --fault portreset  Reset the port underneath a subject that is provably
#                      streaming. The device stays present and the stream dies:
#                      precisely the "silent but present" condition the recovery
#                      exists for, induced deterministically. This is the mode
#                      that yields the measured recovery bound.
#
# In both modes the fault is GATED on frames actually advancing. A blind timer
# can miss the streaming window and "pass" by accident.
#
#   usage: wedge-recovery.sh [options]
#     --fault <mode>          sigkill | portreset       (default sigkill)
#     --kill-mode <mode>      cgroup | holder           (default cgroup)
#     --vid-pid <vvvv:pppp>   subject camera            (default 2ca3:0023)
#     --bus <b-p>             subject USB port          (default 5-1)
#     --control-bus <b-p>     negative control port     (default 10-1)
#     --repeat <n>            cycles                    (default 3)
#     --settle-max-ms <ms>    reset-settle-max-ms bound (default 8000)
#     --outdir <dir>          transcript directory      (default ./wedge-recovery-out)
#
# Must run as root: claiming the UVC interface and resetting the port both need
# it, and on a board with fs.protected_regular the transcript directory must be
# root-owned too (pass --outdir /root/...).
#
# Exit: 0 all exercised cycles recovered inside the bound; 1 a failure or an
# inconclusive run; 77 skipped.

set -u -o pipefail

FAULT=sigkill
KILL_MODE=cgroup
VID_PID=2ca3:0023
BUS=5-1
CONTROL_BUS=10-1
REPEAT=3
SETTLE_MAX_MS=8000
OUTDIR=./wedge-recovery-out

while [ $# -gt 0 ]; do
  case "$1" in
    --fault) FAULT=$2; shift 2 ;;
    --kill-mode) KILL_MODE=$2; shift 2 ;;
    --vid-pid) VID_PID=$2; shift 2 ;;
    --bus) BUS=$2; shift 2 ;;
    --control-bus) CONTROL_BUS=$2; shift 2 ;;
    --repeat) REPEAT=$2; shift 2 ;;
    --settle-max-ms) SETTLE_MAX_MS=$2; shift 2 ;;
    --outdir) OUTDIR=$2; shift 2 ;;
    -h|--help) sed -n '2,55p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

if [ "${CERALIVE_BOARD_TEST:-0}" != "1" ]; then
  echo "SKIP: real hardware required; re-run with CERALIVE_BOARD_TEST=1" >&2
  exit 77
fi
if [ "$(id -u)" -ne 0 ]; then
  echo "FAIL: must run as root (uvc_open needs it: 'Access denied' otherwise)" >&2
  exit 1
fi
for tool in gst-launch-1.0 timeout perl; do
  command -v "$tool" >/dev/null 2>&1 || { echo "FAIL: $tool not found" >&2; exit 1; }
done

mkdir -p "$OUTDIR" || exit 1
RUN_LOG=$OUTDIR/wedge-recovery.log

log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" | tee -a "$RUN_LOG"; }
now_ms() { echo $(($(date +%s%N) / 1000000)); }

# Kernel driver bound to one USB interface, or NONE. `readlink -f` on a missing
# symlink echoes the path itself, so the -L test is what makes NONE truthful.
iface_driver() {
  local path=/sys/bus/usb/devices/$1/driver
  if [ -L "$path" ]; then basename "$(readlink -f "$path")"; else echo NONE; fi
}
drivers_of() { echo "$(iface_driver "$1:1.0")/$(iface_driver "$1:1.1")"; }

# Buffers a `gst-launch -v ... ! fakesink silent=false` run has rendered. Frame
# ADVANCE, not process liveness, is what gates every fault below. (`-q` drops
# these lines entirely, so both runs must use `-v`.) grep -c prints 0 and exits
# 1 on no match, so the status must be swallowed without emitting a second 0.
buffers_in() {
  local n
  n=$(grep -c 'last-message = chain' "$1" 2>/dev/null)
  echo "${n:-0}"
}

# USBDEVFS_RESET, the ioctl libusb_reset_device() issues - from outside the
# element, so the subject's stream dies while the device stays on the bus.
# _IO('U', 20) == 0x5514.
port_reset() {
  local dev=$1 busnum devnum node
  busnum=$(cat "/sys/bus/usb/devices/$dev/busnum") || return 1
  devnum=$(cat "/sys/bus/usb/devices/$dev/devnum") || return 1
  node=$(printf '/dev/bus/usb/%03d/%03d' "$busnum" "$devnum")
  perl -e 'open(my $f, "+<", $ARGV[0]) or die "open: $!";
           ioctl($f, 0x5514, 0) or die "ioctl: $!"; close($f);' "$node"
}

# Every process libuvc's reattach helper would be caught by if systemd tore the
# holder's cgroup down: same cgroup, same comm, reparented to init by the double
# fork. setsid() puts it in its own session, so a process-group kill misses it -
# only a cgroup sweep like this one finds it.
cgroup_peers_of() {
  local holder=$1 cg comm peers="" p pid
  cg=$(head -1 "/proc/$holder/cgroup" 2>/dev/null) || return 0
  comm=$(cat "/proc/$holder/comm" 2>/dev/null)
  for p in /proc/[0-9]*; do
    pid=${p#/proc/}
    [ "$pid" = "$holder" ] && continue
    [ "$(cat "$p/comm" 2>/dev/null)" = "$comm" ] || continue
    [ "$(awk '/^PPid:/{print $2}' "$p/status" 2>/dev/null)" = "1" ] || continue
    [ "$(head -1 "$p/cgroup" 2>/dev/null)" = "$cg" ] || continue
    peers="$peers $pid"
  done
  echo "$peers"
}

start_stream() {
  local logfile=$1 extra=${2:-}
  # shellcheck disable=SC2086
  GST_DEBUG=libuvch264src:5 GST_DEBUG_NO_COLOR=1 \
    timeout 120 gst-launch-1.0 -v libuvch264src index="$VID_PID" $extra \
      ! video/x-h264,width=1920,height=1080,framerate=30/1 \
      ! fakesink silent=false > "$logfile" 2>&1 &
  echo $!
}

# Block until frames are provably advancing AND both interfaces are held.
await_frame_gate() {
  local pid=$1 logfile=$2 deadline b1 b2 d
  deadline=$(($(now_ms) + 30000))
  while [ "$(now_ms)" -lt "$deadline" ]; do
    kill -0 "$pid" 2>/dev/null || return 1
    d=$(drivers_of "$BUS")
    if [ "$d" = "usbfs/usbfs" ]; then
      b1=$(buffers_in "$logfile"); sleep 0.5; b2=$(buffers_in "$logfile")
      if [ "$b1" -gt 0 ] && [ "$b2" -gt "$b1" ]; then
        log "GATE OPEN pid=$pid drivers=$d buffers $b1->$b2 advancing=true"
        return 0
      fi
    fi
    sleep 0.2
  done
  return 1
}

HOLDER_PID=
SUBJECT_PID=
CERASTREAM_WAS_ACTIVE=no

cleanup() {
  [ -n "$HOLDER_PID" ] && kill -9 "$HOLDER_PID" 2>/dev/null
  [ -n "$SUBJECT_PID" ] && kill -15 "$SUBJECT_PID" 2>/dev/null
  sleep 2
  [ -n "$SUBJECT_PID" ] && kill -9 "$SUBJECT_PID" 2>/dev/null
  if [ "$CERASTREAM_WAS_ACTIVE" = yes ]; then
    log "restoring cerastream.service"
    systemctl start cerastream.service 2>/dev/null
  fi
}
trap cleanup EXIT INT TERM

# The engine holds the same camera, and systemd would restart it mid-cycle. Park
# it for the duration; the trap puts it back exactly as it was found.
if systemctl is-active --quiet cerastream.service 2>/dev/null; then
  CERASTREAM_WAS_ACTIVE=yes
  log "stopping cerastream.service for the duration (will be restored)"
  systemctl stop cerastream.service 2>/dev/null
  sleep 3
fi

CONTROL_BEFORE=$(drivers_of "$CONTROL_BUS")
log "fault=$FAULT kill-mode=$KILL_MODE repeat=$REPEAT bound=${SETTLE_MAX_MS}ms"
log "negative control $CONTROL_BUS before: $CONTROL_BEFORE"
log "subject $BUS ($VID_PID) before: $(drivers_of "$BUS")"

FAILURES=0
NO_WEDGE=0
RECOVERY_MS_LIST=()

# Score one subject transcript that has already had its fault injected.
score_subject() {
  local cycle=$1 logfile=$2 allow_no_wedge=$3
  local deadline recovered=no reset_seen=no silence_seen=no ms b1 b2

  deadline=$(($(now_ms) + 90000))
  while [ "$(now_ms)" -lt "$deadline" ]; do
    grep -q 'frames advancing again' "$logfile" 2>/dev/null && { recovered=yes; break; }
    grep -q 'Wedge recovery gave up' "$logfile" 2>/dev/null && break
    kill -0 "$SUBJECT_PID" 2>/dev/null || break
    sleep 0.25
  done
  grep -q 'USB port reset issued' "$logfile" 2>/dev/null && reset_seen=yes
  grep -q 'assuming disconnect' "$logfile" 2>/dev/null && silence_seen=yes

  if [ "$silence_seen" = no ] && [ "$recovered" = no ]; then
    if [ "$allow_no_wedge" = yes ]; then
      log "NO-WEDGE cycle $cycle: device healthy after the fault; recovery not exercised (excluded)"
      NO_WEDGE=$((NO_WEDGE + 1))
      return 0
    fi
    log "FAIL cycle $cycle: the fault never made the stream go silent"
    FAILURES=$((FAILURES + 1))
    return 0
  fi

  if [ "$recovered" != yes ]; then
    log "FAIL cycle $cycle: no recovery (port-reset-issued=$reset_seen)"
    grep -E 'Wedge recovery' "$logfile" | tail -3 | tee -a "$RUN_LOG"
    FAILURES=$((FAILURES + 1))
    return 0
  fi

  ms=$(grep -o 'frames advancing again [0-9]* ms' "$logfile" | head -1 | grep -o '[0-9]*')
  b1=$(buffers_in "$logfile"); sleep 2; b2=$(buffers_in "$logfile")
  if [ "$reset_seen" != yes ]; then
    log "FAIL cycle $cycle: recovered without the real port-reset path"
    FAILURES=$((FAILURES + 1))
  elif [ "$b2" -le "$b1" ]; then
    log "FAIL cycle $cycle: recovery claimed but frames not advancing ($b1->$b2)"
    FAILURES=$((FAILURES + 1))
  elif [ "$ms" -gt "$SETTLE_MAX_MS" ]; then
    log "FAIL cycle $cycle: recovery ${ms}ms EXCEEDS bound ${SETTLE_MAX_MS}ms"
    FAILURES=$((FAILURES + 1))
  else
    log "PASS cycle $cycle: reset-to-advancing-frames=${ms}ms (bound ${SETTLE_MAX_MS}ms), buffers $b1->$b2"
    RECOVERY_MS_LIST+=("$ms")
  fi
}

stop_subject() {
  [ -n "$SUBJECT_PID" ] || return 0
  kill -15 "$SUBJECT_PID" 2>/dev/null; sleep 2
  kill -9 "$SUBJECT_PID" 2>/dev/null
  SUBJECT_PID=
  sleep 3
}

for cycle in $(seq 1 "$REPEAT"); do
  log "=== cycle $cycle/$REPEAT (fault=$FAULT) ==="
  SUBJ_LOG=$OUTDIR/cycle$cycle-subject.log

  if [ "$FAULT" = sigkill ]; then
    HOLD_LOG=$OUTDIR/cycle$cycle-holder.log
    HOLDER_PID=$(start_stream "$HOLD_LOG")
    log "holder pid=$HOLDER_PID"
    if ! await_frame_gate "$HOLDER_PID" "$HOLD_LOG"; then
      log "FAIL cycle $cycle: gate never opened (no hold+advancing-frames window)"
      kill -9 "$HOLDER_PID" 2>/dev/null; HOLDER_PID=
      FAILURES=$((FAILURES + 1)); sleep 5; continue
    fi

    victims=$HOLDER_PID
    if [ "$KILL_MODE" = cgroup ]; then
      peers=$(cgroup_peers_of "$HOLDER_PID")
      victims="$HOLDER_PID$peers"
      log "cgroup sweep: holder=$HOLDER_PID reattach-helper(s)=${peers:-none}"
    fi
    # shellcheck disable=SC2086
    kill -9 $victims 2>/dev/null
    log "T0 KILL -9 [$victims]"
    wait "$HOLDER_PID" 2>/dev/null
    HOLDER_PID=
    sleep 3
    log "post-kill drivers: $(drivers_of "$BUS")"

    SUBJECT_PID=$(start_stream "$SUBJ_LOG" "reset-settle-max-ms=$SETTLE_MAX_MS")
    log "subject pid=$SUBJECT_PID"
    score_subject "$cycle" "$SUBJ_LOG" yes
    stop_subject
  else
    SUBJECT_PID=$(start_stream "$SUBJ_LOG" "reset-settle-max-ms=$SETTLE_MAX_MS")
    log "subject pid=$SUBJECT_PID"
    if ! await_frame_gate "$SUBJECT_PID" "$SUBJ_LOG"; then
      log "FAIL cycle $cycle: gate never opened (subject never streamed)"
      stop_subject; FAILURES=$((FAILURES + 1)); sleep 5; continue
    fi
    if port_reset "$BUS"; then
      log "T0 external USBDEVFS_RESET on $BUS"
    else
      log "FAIL cycle $cycle: could not reset the port"
      stop_subject; FAILURES=$((FAILURES + 1)); continue
    fi
    score_subject "$cycle" "$SUBJ_LOG" no
    stop_subject
  fi
done

CONTROL_AFTER=$(drivers_of "$CONTROL_BUS")
log "negative control $CONTROL_BUS after: $CONTROL_AFTER"
if [ "$CONTROL_AFTER" != "$CONTROL_BEFORE" ]; then
  log "FAIL: negative control $CONTROL_BUS changed: $CONTROL_BEFORE -> $CONTROL_AFTER"
  FAILURES=$((FAILURES + 1))
else
  log "PASS: negative control $CONTROL_BUS unaffected ($CONTROL_AFTER)"
fi

log "subject $BUS after: $(drivers_of "$BUS")"
log "recoveries: ${RECOVERY_MS_LIST[*]:-none} ms"
log "scored cycles: ${#RECOVERY_MS_LIST[@]}; no-wedge (excluded): $NO_WEDGE; failures: $FAILURES"

if [ "$FAILURES" -ne 0 ]; then
  log "RESULT: FAIL ($FAILURES failure(s))"
  exit 1
fi
if [ "${#RECOVERY_MS_LIST[@]}" -eq 0 ]; then
  log "RESULT: INCONCLUSIVE (no cycle exercised the recovery)"
  exit 1
fi
log "RESULT: PASS (${#RECOVERY_MS_LIST[@]} cycle(s) recovered inside ${SETTLE_MAX_MS}ms)"
exit 0
