#!/usr/bin/env bash
#
# UVC negotiation / phantom-mode drill matrix, on real hardware.
#
# NOT part of the ctest suite and deliberately NOT registered in
# tests/CMakeLists.txt: it needs a physical UVC camera, root, and permission to
# replace the installed plugin .so, none of which exist in CI. Run it by hand on
# a board.
#
# What it proves that the mock suite cannot: which modes a REAL camera actually
# negotiates and then actually STREAMS, under a chosen probe policy, with the
# verdict computed here rather than eyeballed out of a log. Every drill emits a
# machine-readable `VERDICT:` line, encodes it in the exit code, and writes a
# `RUN_COMPLETE` sentinel LAST - an evidence directory without that sentinel is
# INCONCLUSIVE by definition, no matter what the logs appear to say.
#
#   usage: negotiation-matrix.sh <subcommand> [options]
#
# ---------------------------------------------------------------------------
# SESSION SUBCOMMANDS (plugin lifecycle - a SESSION, not a per-drill action)
# ---------------------------------------------------------------------------
# A drill never touches the installed .so. Deployment happens once per session,
# and sessions are PER-ENVIRONMENT so an edge session and a vendor-kernel
# session can be open at the same time without clobbering each other (/data is
# shared across slots; the environment-keyed manifest name is what keeps them
# apart).
#
#   session-start --env <edge|vendor> --so <file> [--target <path>]
#                 [--backup-root <dir>]
#       Back up the installed .so (path + sha256) into an environment-keyed
#       manifest, then deploy <file>: stage inside the TARGET directory,
#       re-verify sha256, same-filesystem check, atomic `mv -f`, `ldconfig`,
#       and clear every user's GStreamer registry cache. The manifest records
#       the identity of the rootfs it deployed into (`findmnt / -o SOURCE` plus
#       /etc/machine-id) so recovery can tell "mine" from "another slot's".
#       Refuses to start a second live session for the same env+rootfs.
#
#   session-end --env <edge|vendor> [--backup-root <dir>]
#       Restore that manifest's backup, assert the restored sha256 equals the
#       recorded one, and retire the manifest. After this the board carries
#       exactly the .so it carried before session-start.
#
#   session-recover [--backup-root <dir>]
#       IDEMPOTENT and ROOTFS-BOUND sweep. Scans the backup root and restores
#       ONLY unretired manifests whose recorded root identity matches the
#       CURRENTLY BOOTED root, asserting sha256. A manifest belonging to another
#       environment is never touched: it is reported as
#           PENDING-FOREIGN: <env> <manifest>
#       and stays pending until that environment is booted and session-recover
#       runs there. Safe to run any number of times, from a fresh SSH
#       connection, with no state beyond the manifests themselves.
#       Exit 0 only when nothing is left pending anywhere; exit 1 when a restore
#       failed OR a foreign manifest is still pending (fail closed - the caller
#       must not read "pending elsewhere" as "board is clean").
#
#   session-status [--backup-root <dir>]
#       Read-only listing of every manifest and its disposition. Changes nothing.
#
# ---------------------------------------------------------------------------
# DRILL SUBCOMMANDS (all take --env and FAIL CLOSED on an unverified .so)
# ---------------------------------------------------------------------------
# Every drill resolves the live manifest for --env on the booted rootfs and
# refuses to run unless the currently installed .so's sha256 equals that
# manifest's recorded drill-build sha256. There is no override.
#
#   transition --env <env> [--policy single|double|retry] [--repeat <n>]
#              [--class increase|same|decrease|all] [--vid-pid <v:p>]
#              [--commit-buffers <n>] [--subject-buffers <n>] [--outdir <dir>]
#       (a) TRANSITION matrix - Drills A and B. Prescribed two-run sequences:
#       commit mode X, then negotiate mode Y, counting
#       `Unable to get stream control: Invalid mode` failures per transition
#       class. Classes: `increase` (720p30->1080p30, 1080p30->4K@30),
#       `same` (1080p30->1080p30), `decrease` (4K@30->1080p30, 1080p30->720p30).
#       Per-run probe policy is handed to the element through the
#       LIBUVCH264SRC_PROBE_POLICY environment variable (dev-knob builds only).
#       Emits a per-class VERDICT plus, when the increase class ran, the
#       Rule A-branch line:
#           RULE_A_BRANCH: REPRODUCED|NOT_REPRODUCED|AMBIGUOUS ...
#       REPRODUCED at an increase-failure rate >= 30%, NOT_REPRODUCED at zero
#       failures, AMBIGUOUS in between. On the conservative branch (policy
#       single, NOT_REPRODUCED or AMBIGUOUS) it also records the status-quo
#       final lines `RULE_G: G3 provenance=status-quo` and
#       `RULE_C_FINAL: C2 rate=248832000 provenance=status-quo-no-rule-c`.
#       NOTE ON EXIT CODES: this drill's VERDICT is PASS only when ZERO
#       negotiation failures were recorded. A Drill A baseline that REPRODUCES
#       the defect therefore ends VERDICT: FAIL / exit 1 - that is the honest
#       machine statement about the single-probe policy, not a harness error.
#       Read RULE_A_BRANCH for the branch decision, never the exit code.
#
#   phantom --env <env> --mode <WxH@fps> [--mode ...] [--policy <p>]
#           [--repeat <n>] [--arm after-smaller|cold-start|after-replug]
#           [--smaller-mode <WxH@fps>] [--buffers <n>] [--vid-pid <v:p>]
#           [--outdir <dir>]
#       (b) PHANTOM matrix - Drill C. Per-mode bounded runs (default
#       num-buffers=300) with access units counted at an `identity` probe, SPS
#       geometry read back from the `h264parse` src caps, fps measured across
#       the first-to-last AU PTS span, and an element-level GST_ERROR/GST_WARN
#       scan. Full process wall time remains in the evidence. Arms:
#         after-smaller  commit --smaller-mode first, then the subject mode
#         cold-start     one operator camera power-cycle per replicate
#         after-replug   one operator physical replug per replicate
#
#   replug     --env <env> --mode <WxH@fps> ...     alias for `phantom --arm after-replug`
#   cold-start --env <env> --mode <WxH@fps> ...     alias for `phantom --arm cold-start`
#       (d) REPLUG / COLD-START arms. Each replicate asks the operator for its
#       OWN physical stimulus through a typed confirmation prompt (`read -r` on
#       /dev/tty). With CERALIVE_BOARD_UNATTENDED=1 these arms are SKIPPED and
#       recorded as `ARM_SKIPPED: ... reason=unattended` with the drill VERDICT
#       forced to INCONCLUSIVE - a skipped arm is NEVER counted as a pass.
#
#   sustained --env <env> --mode <WxH@fps> [--minutes <n>] [--repeat <n>]
#             [--warm] [--policy <p>] [--thermal-interval <s>] [--outdir <dir>]
#       (c) SUSTAINED mode. 3-5 minute runs with /sys/class/thermal sampled
#       throughout. The bounded 300/300 clause of the per-run pass rule is
#       replaced by "survived the full window"; every other clause is identical.
#
# ---------------------------------------------------------------------------
# PER-RUN PASS RULE (computed HERE, never by the caller)
# ---------------------------------------------------------------------------
#   AU count == requested count (300/300 by default, at the identity probe)
#   AND (AU_count - 1) / (last_AU_PTS - first_AU_PTS) >= 0.90 * nominal_fps
#   AND SPS-derived geometry (h264parse caps) == requested geometry
#   AND zero element-level GST_ERROR / GST_WARN lines
# If fewer than two AUs carry parseable `H:MM:SS.nanoseconds` PTS values, the
# fps calculation falls back explicitly to AU_count / wall_seconds.
# A run missing any provenance field, or whose transcript could not be scored,
# is INCONCLUSIVE - never a pass.
#
# ---------------------------------------------------------------------------
# VERDICT SUBCOMMAND (campaign aggregates - also computed HERE)
# ---------------------------------------------------------------------------
#   verdict --rule g --run <dir> [--run <dir> ...]
#       Rule G: G1 iff every `retry`-policy transition run recorded ZERO
#       failures across all classes at N >= 10 per class; otherwise G3.
#       G2 does not exist and is never emitted.
#
#   verdict --rule c --run <dir> [--run <dir> ...] [--vendor-run <dir> ...]
#           [--sustained-run <dir> ...]
#       Rule C: C1 (delete the cap) iff every authorizing cell
#       (3840x2160@{60,50,48} x {after-smaller,cold-start,after-replug}) passed
#       with zero failures at the pre-registered N, no arm was skipped, nothing
#       was INCONCLUSIVE, at least 3 sustained runs passed, AND a vendor-gate
#       run set was supplied and passed. Otherwise C2 at the highest CONTIGUOUS
#       safe ceiling from the candidate list - never a ceiling that re-admits a
#       failed lower mode. Emits the provisional aggregate and the final line:
#           RULE_C_AGGREGATE: C1|C2 rate=<r>
#           RULE_C_FINAL: C1|C2 rate=<r> provenance=<...>
#
# ---------------------------------------------------------------------------
# ENVIRONMENT
# ---------------------------------------------------------------------------
#   CERALIVE_BOARD_TEST=1        REQUIRED. Without it every subcommand exits 77.
#   CERALIVE_BOARD_UNATTENDED=1  Skip operator-prompt arms with a distinct
#                                recorded status (never a silent pass).
#   LIBUVCH264SRC_PROBE_POLICY   Set per run by this script from --policy.
#
# Must run as root: claiming the UVC interface, replacing the installed .so and
# writing USB sysfs all need it.
#
# Output goes to --outdir (default ./test-results/negotiation-matrix, which is
# gitignored in this repo). On a board, point it at /data - never /root, whose
# filesystem has filled during a prior campaign.
#
# Exit: 0 PASS / clean, 1 FAIL or INCONCLUSIVE or a harness refusal, 77 skipped
# (gate unset, or a mode that cannot run in this environment).

set -u -o pipefail

# --- defaults ---------------------------------------------------------------

VID_PID=2ca3:0023
POLICY=default
OUTDIR=./test-results/negotiation-matrix
BACKUP_ROOT=/data/deploy-backups
ENVNAME=
SO_SRC=
TARGET_SO=
REPEAT=
CLASSES=all
ARM=after-smaller
SMALLER_MODE=1920x1080@30
BUFFERS=300
COMMIT_BUFFERS=30
SUBJECT_BUFFERS=30
MINUTES=4
THERMAL_INTERVAL=15
WARM=no
MODES=()
RULE=
RUN_DIRS=()
VENDOR_RUN_DIRS=()
SUSTAINED_RUN_DIRS=()

PLUGIN_PKG=gstreamer1.0-libuvch264src
PLUGIN_SO_NAME=libgstlibuvch264src.so
MANIFEST_SCHEMA=1

# The advertised 3840x2160 ladder and its pixel rates. These are the ONLY
# ceilings Rule C may name; a ceiling that re-admits a failed lower mode is not
# in the list by construction.
CANDIDATE_MODES=(3840x2160@30 3840x2160@48 3840x2160@50 3840x2160@60)
CANDIDATE_RATES=(248832000 398131200 414720000 497664000)
STATUS_QUO_RATE=248832000

# Pre-registered replicate counts per authorizing cell (Rule C).
N_AFTER_SMALLER=45
N_COLD_START=10
N_AFTER_REPLUG=5
N_TRANSITION_CLASS=10

usage() { sed -n '2,162p' "$0"; }

# --- tiny helpers -----------------------------------------------------------

ts_utc() { date -u +%Y-%m-%dT%H:%M:%SZ; }
now_ms() { echo $(($(date +%s%N) / 1000000)); }

run_rate_metrics() {
  local logfile=$1 aus=$2 wall_s=$3 stream_s fps

  stream_s=$(awk -v expected="$aus" '
    /GstIdentity:auprobe: last-message = chain/ {
      chain_count++
      if (match($0, /pts: [0-9]+:[0-9][0-9]:[0-9][0-9]\.[0-9]+,/)) {
        token = substr($0, RSTART + 5, RLENGTH - 6)
        if (split(token, hms, ":") != 3 || split(hms[3], second_parts, ".") != 2 ||
            length(second_parts[2]) != 9 || hms[2] >= 60 || second_parts[1] >= 60) {
          next
        }
        pts = (hms[1] * 3600) + (hms[2] * 60) + hms[3]
        parsed_count++
        if (parsed_count == 1) {
          first_pts = pts
        }
        last_pts = pts
      }
    }
    END {
      if (chain_count != expected || parsed_count != chain_count || parsed_count < 2 ||
          last_pts <= first_pts) {
        exit 1
      }
      printf "%.9f", last_pts - first_pts
    }
  ' "$logfile" 2>/dev/null) || stream_s=

  if [ -n "$stream_s" ]; then
    fps=$(awk -v a="$aus" -v s="$stream_s" \
      'BEGIN { printf "%.3f", (a - 1) / s }')
    printf '%s %s pts-span\n' "$stream_s" "$fps"
    return 0
  fi

  fps=$(awk -v a="$aus" -v s="$wall_s" \
    'BEGIN { if (s <= 0) print "0.000"; else printf "%.3f", a / s }')
  printf 'unavailable %s wall-fallback\n' "$fps"
}

RUN_LOG=
log() {
  local line
  line="$(ts_utc) $*"
  if [ -n "$RUN_LOG" ]; then printf '%s\n' "$line" | tee -a "$RUN_LOG"; else printf '%s\n' "$line"; fi
}

die() { log "FAIL: $*"; exit 1; }

# Prefer a real uuid source, fall back to the kernel's, then to a timestamp mix.
# A run without a stable unique id cannot be provenance-stamped, so this must
# never silently return empty.
gen_uuid() {
  local u=
  if command -v uuidgen >/dev/null 2>&1; then u=$(uuidgen 2>/dev/null); fi
  if [ -z "$u" ] && [ -r /proc/sys/kernel/random/uuid ]; then u=$(cat /proc/sys/kernel/random/uuid); fi
  if [ -z "$u" ]; then u=$(printf '%s-%s-%s' "$(date -u +%Y%m%d%H%M%S)" "$$" "$RANDOM"); fi
  printf '%s' "$u"
}

sha256_of() {
  [ -f "$1" ] || { printf ''; return 1; }
  sha256sum "$1" 2>/dev/null | awk '{print $1}'
}

# Split "3840x2160@60" into the three numbers callers actually need.
mode_width()  { printf '%s' "${1%%x*}"; }
mode_height() { local r=${1#*x}; printf '%s' "${r%%@*}"; }
mode_fps()    { printf '%s' "${1##*@}"; }

mode_valid() {
  case "$1" in
    [0-9]*x[0-9]*@[0-9]*) return 0 ;;
    *) return 1 ;;
  esac
}

# --- gates ------------------------------------------------------------------

require_gate() {
  if [ "${CERALIVE_BOARD_TEST:-0}" != "1" ]; then
    echo "SKIP: real hardware required; re-run with CERALIVE_BOARD_TEST=1" >&2
    exit 77
  fi
}

require_root() {
  if [ "$(id -u)" -ne 0 ]; then
    echo "FAIL: must run as root (uvc_open and the .so swap both need it)" >&2
    exit 1
  fi
}

require_tools() {
  local tool
  for tool in "$@"; do
    command -v "$tool" >/dev/null 2>&1 || { echo "FAIL: $tool not found" >&2; exit 1; }
  done
}

require_env_name() {
  case "${ENVNAME:-}" in
    edge|vendor) : ;;
    "") echo "FAIL: --env <edge|vendor> is required" >&2; exit 1 ;;
    *) echo "FAIL: --env must be edge or vendor (got '$ENVNAME')" >&2; exit 1 ;;
  esac
}

# --- rootfs identity --------------------------------------------------------
#
# /data is shared across RAUC slots, so a manifest alone does not say which
# rootfs it deployed into. The root block device plus /etc/machine-id do, and
# both are readable from a fresh SSH connection with no prior state.

root_source() {
  local s=
  if command -v findmnt >/dev/null 2>&1; then s=$(findmnt / -o SOURCE -n 2>/dev/null | awk '{print $1}'); fi
  if [ -z "$s" ]; then s=$(awk '$2 == "/" {print $1; exit}' /proc/mounts 2>/dev/null); fi
  printf '%s' "${s:-unknown}"
}

machine_id() {
  local m=
  [ -r /etc/machine-id ] && m=$(tr -d '[:space:]' < /etc/machine-id)
  printf '%s' "${m:-unknown}"
}

# --- plugin target resolution ----------------------------------------------

resolve_target_so() {
  local p='' c
  if [ -n "$TARGET_SO" ]; then printf '%s' "$TARGET_SO"; return 0; fi
  if command -v dpkg >/dev/null 2>&1; then
    p=$(dpkg -L "$PLUGIN_PKG" 2>/dev/null | grep -E "/${PLUGIN_SO_NAME}\$" | head -1)
  fi
  if [ -z "$p" ]; then
    for c in /usr/lib/*/gstreamer-1.0/"$PLUGIN_SO_NAME" /lib/*/gstreamer-1.0/"$PLUGIN_SO_NAME"; do
      [ -f "$c" ] || continue
      p=$c
      break
    done
  fi
  printf '%s' "$p"
}

clear_registry_caches() {
  local d
  for d in /root /home/*; do
    [ -d "$d" ] || continue
    rm -f "$d"/.cache/gstreamer-1.0/registry.*.bin 2>/dev/null
  done
  return 0
}

# --- manifests --------------------------------------------------------------
#
# Flat key=value so a human on a serial console can read one, and so recovery
# needs nothing but grep.

manifest_get() {
  local file=$1 key=$2
  awk -F= -v k="$key" '$1 == k { sub(/^[^=]*=/, ""); print; exit }' "$file" 2>/dev/null
}

manifest_set() {
  local file=$1 key=$2 value=$3 tmp
  tmp=$(mktemp "${file}.XXXXXX") || return 1
  awk -F= -v k="$key" -v v="$value" '
    $1 == k { print k "=" v; found = 1; next }
    { print }
    END { if (!found) print k "=" v }
  ' "$file" > "$tmp" || { rm -f "$tmp"; return 1; }
  mv -f "$tmp" "$file"
}

manifest_is_live() {
  local file=$1
  [ -f "$file" ] || return 1
  [ "$(manifest_get "$file" retired)" = "no" ]
}

manifest_is_local() {
  local file=$1
  [ "$(manifest_get "$file" root_source)" = "$(root_source)" ] &&
    [ "$(manifest_get "$file" machine_id)" = "$(machine_id)" ]
}

list_manifests() {
  ls -1 "$BACKUP_ROOT"/*/session-*.manifest 2>/dev/null
}

# The one live manifest for an environment on THIS rootfs, or empty.
find_live_manifest() {
  local want_env=$1 f
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    manifest_is_live "$f" || continue
    manifest_is_local "$f" || continue
    [ "$(manifest_get "$f" env)" = "$want_env" ] || continue
    printf '%s' "$f"
    return 0
  done <<< "$(list_manifests)"
  printf ''
  return 1
}

# Stage inside the TARGET directory so the final `mv -f` is a same-filesystem
# rename. Staging in /data and moving across filesystems is a copy, not an
# atomic swap, and a half-written plugin is exactly the state a drill must
# never observe.
install_so_atomic() {
  local src=$1 target=$2 want_sha=$3
  local dir staged got src_dev staged_dev
  dir=$(dirname "$target")
  [ -d "$dir" ] || { log "install: target directory $dir does not exist"; return 1; }
  staged="$dir/.negotiation-matrix.$$.staged"

  cp -f "$src" "$staged" || { log "install: staging copy failed"; return 1; }
  got=$(sha256_of "$staged")
  if [ "$got" != "$want_sha" ]; then
    rm -f "$staged"
    log "install: staged sha256 mismatch (want $want_sha got ${got:-none})"
    return 1
  fi
  src_dev=$(stat -c %d "$dir" 2>/dev/null)
  staged_dev=$(stat -c %d "$staged" 2>/dev/null)
  if [ -z "$src_dev" ] || [ "$src_dev" != "$staged_dev" ]; then
    rm -f "$staged"
    log "install: staging file is not on the target filesystem (dev $staged_dev vs $src_dev)"
    return 1
  fi
  chmod 0644 "$staged" 2>/dev/null
  mv -f "$staged" "$target" || { rm -f "$staged"; log "install: atomic mv failed"; return 1; }
  ldconfig 2>/dev/null
  clear_registry_caches
  got=$(sha256_of "$target")
  if [ "$got" != "$want_sha" ]; then
    log "install: post-install sha256 mismatch (want $want_sha got ${got:-none})"
    return 1
  fi
  return 0
}

# --- session-start ----------------------------------------------------------

cmd_session_start() {
  require_gate; require_env_name; require_root; require_tools sha256sum stat
  [ -n "$SO_SRC" ] || die "session-start needs --so <drill .so>"
  [ -f "$SO_SRC" ] || die "session-start: --so '$SO_SRC' does not exist"

  local target existing drill_sha backup_sha uuid date_dir manifest backup_path
  target=$(resolve_target_so)
  [ -n "$target" ] || die "cannot resolve the installed $PLUGIN_SO_NAME (pass --target)"
  [ -f "$target" ] || die "resolved target '$target' does not exist"

  existing=$(find_live_manifest "$ENVNAME")
  if [ -n "$existing" ]; then
    log "REFUSED: a live '$ENVNAME' session already exists on this rootfs: $existing"
    log "run 'session-end --env $ENVNAME' or 'session-recover' first"
    exit 1
  fi

  drill_sha=$(sha256_of "$SO_SRC") || die "cannot hash --so '$SO_SRC'"
  backup_sha=$(sha256_of "$target") || die "cannot hash installed '$target'"
  uuid=$(gen_uuid)
  date_dir="$BACKUP_ROOT/$(date -u +%Y-%m-%d)"
  mkdir -p "$date_dir" || die "cannot create $date_dir"
  manifest="$date_dir/session-$ENVNAME-$uuid.manifest"
  backup_path="$date_dir/$PLUGIN_SO_NAME.$ENVNAME.$uuid.orig"

  cp -f "$target" "$backup_path" || die "backup copy to $backup_path failed"
  if [ "$(sha256_of "$backup_path")" != "$backup_sha" ]; then
    die "backup sha256 mismatch immediately after copy"
  fi

  # The manifest is written BEFORE the swap and while it still describes an
  # untouched board. A crash between here and the mv leaves a manifest whose
  # restore is a harmless no-op; a crash the other way round would leave a
  # drill .so nobody can attribute or undo.
  {
    echo "schema=$MANIFEST_SCHEMA"
    echo "env=$ENVNAME"
    echo "uuid=$uuid"
    echo "created_utc=$(ts_utc)"
    echo "hostname=$(hostname 2>/dev/null)"
    echo "root_source=$(root_source)"
    echo "machine_id=$(machine_id)"
    echo "target_path=$target"
    echo "backup_path=$backup_path"
    echo "backup_sha256=$backup_sha"
    echo "drill_source=$SO_SRC"
    echo "drill_sha256=$drill_sha"
    echo "deployed=pending"
    echo "retired=no"
    echo "retired_utc="
  } > "$manifest" || die "cannot write manifest $manifest"

  RUN_LOG="$date_dir/session-$ENVNAME-$uuid.log"
  log "session-start env=$ENVNAME uuid=$uuid"
  log "target=$target"
  log "backup=$backup_path sha256=$backup_sha"
  log "drill=$SO_SRC sha256=$drill_sha"
  log "rootfs root_source=$(root_source) machine_id=$(machine_id)"

  if ! install_so_atomic "$SO_SRC" "$target" "$drill_sha"; then
    log "deploy failed; restoring the backup before giving up"
    install_so_atomic "$backup_path" "$target" "$backup_sha" ||
      log "RESTORE ALSO FAILED - board carries an unverified .so at $target"
    manifest_set "$manifest" retired yes
    manifest_set "$manifest" retired_utc "$(ts_utc)"
    die "session-start could not deploy the drill .so"
  fi

  manifest_set "$manifest" deployed yes
  log "SESSION_START: env=$ENVNAME manifest=$manifest drill_sha256=$drill_sha"
  log "VERDICT: PASS session-start env=$ENVNAME"
  exit 0
}

# --- restore primitive shared by session-end and session-recover -------------

restore_manifest() {
  local manifest=$1
  local target backup backup_sha got
  target=$(manifest_get "$manifest" target_path)
  backup=$(manifest_get "$manifest" backup_path)
  backup_sha=$(manifest_get "$manifest" backup_sha256)

  if [ -z "$target" ] || [ -z "$backup" ] || [ -z "$backup_sha" ]; then
    log "restore: manifest $manifest is missing target/backup fields"
    return 1
  fi
  if [ ! -f "$backup" ]; then
    log "restore: backup $backup is gone; cannot restore $target"
    return 1
  fi
  if [ "$(sha256_of "$backup")" != "$backup_sha" ]; then
    log "restore: backup $backup no longer matches its recorded sha256"
    return 1
  fi
  install_so_atomic "$backup" "$target" "$backup_sha" || return 1
  got=$(sha256_of "$target")
  if [ "$got" != "$backup_sha" ]; then
    log "restore: post-restore sha256 mismatch (want $backup_sha got ${got:-none})"
    return 1
  fi
  manifest_set "$manifest" retired yes
  manifest_set "$manifest" retired_utc "$(ts_utc)"
  log "RESTORED: $target sha256=$backup_sha manifest=$manifest"
  return 0
}

# --- session-end ------------------------------------------------------------

cmd_session_end() {
  require_gate; require_env_name; require_root; require_tools sha256sum stat

  local manifest
  manifest=$(find_live_manifest "$ENVNAME")
  if [ -z "$manifest" ]; then
    log "VERDICT: FAIL session-end env=$ENVNAME reason=no-live-session-on-this-rootfs"
    log "hint: 'session-status' lists every manifest; a foreign one needs its own environment booted"
    exit 1
  fi
  RUN_LOG="$(dirname "$manifest")/session-$ENVNAME-$(manifest_get "$manifest" uuid).log"
  log "session-end env=$ENVNAME manifest=$manifest"

  if ! restore_manifest "$manifest"; then
    log "VERDICT: FAIL session-end env=$ENVNAME reason=restore-failed manifest=$manifest"
    exit 1
  fi
  log "SESSION_END: env=$ENVNAME manifest=$manifest"
  log "VERDICT: PASS session-end env=$ENVNAME"
  exit 0
}

# --- session-recover --------------------------------------------------------

cmd_session_recover() {
  require_gate; require_root; require_tools sha256sum stat

  local f recovered=0 failed=0 foreign=0 seen=0 env_of
  log "session-recover on root_source=$(root_source) machine_id=$(machine_id)"
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    seen=$((seen + 1))
    env_of=$(manifest_get "$f" env)
    if ! manifest_is_live "$f"; then
      log "SKIP-RETIRED: ${env_of:-unknown} $f"
      continue
    fi
    if ! manifest_is_local "$f"; then
      log "PENDING-FOREIGN: ${env_of:-unknown} $f"
      foreign=$((foreign + 1))
      continue
    fi
    if restore_manifest "$f"; then
      log "RECOVERED: ${env_of:-unknown} $f"
      recovered=$((recovered + 1))
    else
      log "RECOVER-FAILED: ${env_of:-unknown} $f"
      failed=$((failed + 1))
    fi
  done <<< "$(list_manifests)"

  log "SESSION_RECOVER: seen=$seen local_recovered=$recovered local_failed=$failed pending_foreign=$foreign"
  if [ "$failed" -ne 0 ]; then
    log "VERDICT: FAIL session-recover reason=restore-failed count=$failed"
    exit 1
  fi
  if [ "$foreign" -ne 0 ]; then
    # Not a failure of this sweep, but the board is NOT clean. Fail closed so a
    # caller can never read "pending elsewhere" as "nothing left to do".
    log "VERDICT: FAIL session-recover reason=pending-foreign count=$foreign"
    log "boot each named environment and run session-recover there before final stop"
    exit 1
  fi
  log "VERDICT: PASS session-recover reason=clean recovered=$recovered"
  exit 0
}

# --- session-status ---------------------------------------------------------

cmd_session_status() {
  require_gate
  local f state
  printf 'rootfs root_source=%s machine_id=%s\n' "$(root_source)" "$(machine_id)"
  while IFS= read -r f; do
    [ -n "$f" ] || continue
    if ! manifest_is_live "$f"; then state=RETIRED
    elif manifest_is_local "$f"; then state=LIVE-LOCAL
    else state=PENDING-FOREIGN
    fi
    printf '%s env=%s target=%s drill_sha256=%s %s\n' \
      "$state" "$(manifest_get "$f" env)" "$(manifest_get "$f" target_path)" \
      "$(manifest_get "$f" drill_sha256)" "$f"
  done <<< "$(list_manifests)"
  exit 0
}

# --- drill precondition: the installed .so must be THIS session's -----------

DRILL_MANIFEST=
DRILL_TARGET=
DRILL_SHA=

assert_verified_so() {
  local installed
  DRILL_MANIFEST=$(find_live_manifest "$ENVNAME")
  if [ -z "$DRILL_MANIFEST" ]; then
    log "VERDICT: FAIL reason=no-live-session env=$ENVNAME"
    log "a drill never deploys; run 'session-start --env $ENVNAME --so <file>' first"
    exit 1
  fi
  DRILL_TARGET=$(manifest_get "$DRILL_MANIFEST" target_path)
  DRILL_SHA=$(manifest_get "$DRILL_MANIFEST" drill_sha256)
  installed=$(sha256_of "$DRILL_TARGET")
  if [ -z "$installed" ] || [ "$installed" != "$DRILL_SHA" ]; then
    log "VERDICT: FAIL reason=unverified-so env=$ENVNAME target=$DRILL_TARGET"
    log "installed sha256=${installed:-none} expected=${DRILL_SHA:-none}"
    exit 1
  fi
  log "so-verified env=$ENVNAME target=$DRILL_TARGET sha256=$DRILL_SHA"
}

# --- cerastream parking (per-drill trap restores service state only) --------

CERASTREAM_WAS_ACTIVE=no
GST_PID=

park_cerastream() {
  if systemctl is-active --quiet cerastream.service 2>/dev/null; then
    CERASTREAM_WAS_ACTIVE=yes
    log "stopping cerastream.service for the duration (will be restored)"
    systemctl stop cerastream.service 2>/dev/null
    sleep 3
  fi
}

# Reached only through the EXIT/INT/TERM trap the drills install, so no static
# caller exists for the linter to find.
# shellcheck disable=SC2317
drill_cleanup() {
  [ -n "$GST_PID" ] && kill -15 "$GST_PID" 2>/dev/null
  sleep 1
  [ -n "$GST_PID" ] && kill -9 "$GST_PID" 2>/dev/null
  if [ "$CERASTREAM_WAS_ACTIVE" = yes ]; then
    log "restoring cerastream.service"
    systemctl start cerastream.service 2>/dev/null
  fi
}

# --- provenance -------------------------------------------------------------

PROVENANCE_COMPLETE=no

thermal_snapshot() {
  local z out=""
  for z in /sys/class/thermal/thermal_zone*; do
    [ -r "$z/temp" ] || continue
    out="$out $(basename "$z")=$(cat "$z/temp" 2>/dev/null)"
  done
  printf '%s' "${out# }"
}

camera_descriptor_field() {
  local field=$1
  lsusb -v -d "$VID_PID" 2>/dev/null | awk -v f="$field" '$1 == f { $1 = ""; sub(/^ +/, ""); print; exit }'
}

libuvc_identity() {
  local so path out=""
  so=$(ldconfig -p 2>/dev/null | awk '/libuvc\.so/ { print $NF; exit }')
  if [ -n "$so" ]; then
    path=$(readlink -f "$so" 2>/dev/null)
    out="path=$path sha256=$(sha256_of "$path" 2>/dev/null)"
    # The fork stamps its version string into the library; keep it best-effort
    # rather than making provenance depend on `strings` being installed.
    if command -v strings >/dev/null 2>&1; then
      out="$out version=$(strings "$path" 2>/dev/null | grep -m1 -E '^libuvc [0-9]|^[0-9]+\.[0-9]+\.[0-9]+$')"
    fi
  fi
  printf '%s' "${out:-unavailable}"
}

# Writes the mandatory provenance block and sets PROVENANCE_COMPLETE. A missing
# field is recorded as <unavailable> AND downgrades the run to INCONCLUSIVE -
# evidence that cannot say which binary produced it is not evidence.
emit_provenance() {
  local file="$1/provenance.txt" missing=0 v
  {
    echo "run_uuid=$RUN_UUID"
    echo "started_utc=$RUN_STARTED_UTC"
    echo "hostname=$(hostname 2>/dev/null)"
    echo "uname=$(uname -a 2>/dev/null)"
    echo "os_release=$(awk -F= '/^PRETTY_NAME=/ { gsub(/"/, "", $2); print $2; exit }' /etc/os-release 2>/dev/null)"
    echo "env=$ENVNAME"
    echo "policy=$POLICY"
    echo "so_path=$DRILL_TARGET"
    echo "so_sha256=$(sha256_of "$DRILL_TARGET")"
    echo "session_manifest=$DRILL_MANIFEST"
    echo "root_source=$(root_source)"
    echo "machine_id=$(machine_id)"
    echo "camera_vid_pid=$VID_PID"
    echo "camera_bcd_device=$(camera_descriptor_field bcdDevice)"
    echo "camera_serial=$(camera_descriptor_field iSerial)"
    echo "camera_firmware=$(camera_descriptor_field iProduct)"
    echo "libuvc=$(libuvc_identity)"
    echo "thermal_start=$(thermal_snapshot)"
    echo "unattended=${CERALIVE_BOARD_UNATTENDED:-0}"
    echo "--- ldd $DRILL_TARGET ---"
    ldd "$DRILL_TARGET" 2>&1
    echo "--- lsusb -t ---"
    lsusb -t 2>&1
    echo "--- lsusb -d $VID_PID ---"
    lsusb -d "$VID_PID" 2>&1
  } > "$file"

  for v in run_uuid hostname uname os_release so_path so_sha256 \
           camera_bcd_device camera_serial camera_firmware libuvc thermal_start; do
    if [ -z "$(manifest_get "$file" "$v")" ]; then
      log "PROVENANCE-MISSING: $v"
      missing=$((missing + 1))
    fi
  done
  if grep -q '^--- lsusb -t ---$' "$file" && [ -s "$file" ]; then :; else missing=$((missing + 1)); fi

  if [ "$missing" -eq 0 ]; then
    PROVENANCE_COMPLETE=yes
    log "PROVENANCE: complete file=$file"
  else
    PROVENANCE_COMPLETE=no
    log "PROVENANCE: INCOMPLETE missing=$missing file=$file"
  fi
}

# --- run directory ----------------------------------------------------------

RUN_UUID=
RUN_STARTED_UTC=
RUNDIR=
SUMMARY=

open_rundir() {
  local mode=$1
  RUN_UUID=$(gen_uuid)
  RUN_STARTED_UTC=$(ts_utc)
  RUNDIR="$OUTDIR/$mode-$(date -u +%Y%m%dT%H%M%SZ)-${RUN_UUID:0:8}"
  mkdir -p "$RUNDIR" || { echo "FAIL: cannot create $RUNDIR" >&2; exit 1; }
  RUN_LOG="$RUNDIR/drill.log"
  SUMMARY="$RUNDIR/summary.txt"
  : > "$SUMMARY"
  log "run uuid=$RUN_UUID mode=$mode dir=$RUNDIR"
}

record() { printf '%s\n' "$*" >> "$SUMMARY"; log "$*"; }

# The sentinel is written LAST, after every other artifact, so its absence is an
# unambiguous INCONCLUSIVE rather than an ambiguous "maybe the run was fine".
seal_rundir() {
  local verdict=$1
  log "sealing $RUNDIR - RUN_COMPLETE is the last file this run writes"
  {
    echo "run_uuid=$RUN_UUID"
    echo "verdict=$verdict"
    echo "finished_utc=$(ts_utc)"
    echo "thermal_end=$(thermal_snapshot)"
  } > "$RUNDIR/RUN_COMPLETE"
}

finish() {
  local verdict=$1
  record "VERDICT: $verdict"
  seal_rundir "$verdict"
  case "$verdict" in
    PASS) exit 0 ;;
    *) exit 1 ;;
  esac
}

# --- one gst-launch run + the pass rule ------------------------------------
#
# Returns 0 PASS, 1 FAIL, 2 INCONCLUSIVE. Every number in the verdict is
# computed here; nothing is left for a reader to infer.

RUN_AUS=0
RUN_WALL_S=0
RUN_STREAM_S=unavailable
RUN_FPS=0
RUN_FPS_SOURCE=wall-fallback
RUN_GEOM=
RUN_ERRORS=0
RUN_INVALID_MODE=0

gst_run() {
  local logfile=$1 mode=$2 want_aus=$3 limit_secs=${4:-0}
  local w h f t0 t1 caps_line
  w=$(mode_width "$mode"); h=$(mode_height "$mode"); f=$(mode_fps "$mode")

  RUN_AUS=0; RUN_WALL_S=0; RUN_STREAM_S=unavailable; RUN_FPS=0
  RUN_FPS_SOURCE=wall-fallback; RUN_GEOM=; RUN_ERRORS=0; RUN_INVALID_MODE=0

  local -a limiter=()
  local -a srcprops=()
  if [ "$want_aus" -gt 0 ]; then
    srcprops=(num-buffers="$want_aus")
    limiter=(timeout $((want_aus / (f > 0 ? f : 30) + 120)))
  else
    limiter=(timeout $((limit_secs + 30)))
  fi

  t0=$(now_ms)
  if [ "$want_aus" -gt 0 ]; then
    LIBUVCH264SRC_PROBE_POLICY="$POLICY" GST_DEBUG=libuvch264src:3 GST_DEBUG_NO_COLOR=1 \
      "${limiter[@]}" gst-launch-1.0 -v libuvch264src index="$VID_PID" "${srcprops[@]}" \
        ! video/x-h264,width="$w",height="$h",framerate="$f"/1 \
        ! identity name=auprobe silent=false \
        ! h264parse ! fakesink sync=false > "$logfile" 2>&1
  else
    LIBUVCH264SRC_PROBE_POLICY="$POLICY" GST_DEBUG=libuvch264src:3 GST_DEBUG_NO_COLOR=1 \
      "${limiter[@]}" gst-launch-1.0 -v -e libuvch264src index="$VID_PID" \
        ! video/x-h264,width="$w",height="$h",framerate="$f"/1 \
        ! identity name=auprobe silent=false \
        ! h264parse ! fakesink sync=false > "$logfile" 2>&1 &
    GST_PID=$!
    sleep "$limit_secs"
    kill -INT "$GST_PID" 2>/dev/null
    wait "$GST_PID" 2>/dev/null
    GST_PID=
  fi
  t1=$(now_ms)

  RUN_WALL_S=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f", (b - a) / 1000.0 }')
  # grep -c prints 0 and exits 1 on no match; swallow the status, keep the zero.
  RUN_AUS=$(grep -c 'GstIdentity:auprobe: last-message = chain' "$logfile" 2>/dev/null)
  RUN_AUS=${RUN_AUS:-0}
  RUN_INVALID_MODE=$(grep -c 'Unable to get stream control: Invalid mode' "$logfile" 2>/dev/null)
  RUN_INVALID_MODE=${RUN_INVALID_MODE:-0}
  RUN_ERRORS=$(grep -cE '[[:space:]](ERROR|WARN)[[:space:]]+libuvch264src[[:space:]]' "$logfile" 2>/dev/null)
  RUN_ERRORS=${RUN_ERRORS:-0}

  # SPS-derived geometry: h264parse republishes width/height parsed out of the
  # SPS on its src pad, which is why the geometry claim is read from THERE and
  # not from the caps we asked for.
  caps_line=$(grep -E 'GstH264Parse:[^:]*\.GstPad:src: caps = ' "$logfile" 2>/dev/null | tail -1)
  if [ -n "$caps_line" ]; then
    local gw gh
    gw=$(printf '%s' "$caps_line" | grep -o 'width=(int)[0-9]*' | head -1 | grep -o '[0-9]*$')
    gh=$(printf '%s' "$caps_line" | grep -o 'height=(int)[0-9]*' | head -1 | grep -o '[0-9]*$')
    [ -n "$gw" ] && [ -n "$gh" ] && RUN_GEOM="${gw}x${gh}"
  fi

  read -r RUN_STREAM_S RUN_FPS RUN_FPS_SOURCE < <(
    run_rate_metrics "$logfile" "$RUN_AUS" "$RUN_WALL_S"
  )
  return 0
}

# The pass rule, verbatim and in one place.
score_run() {
  local mode=$1 want_aus=$2 kind=${3:-bounded}
  local w h f floor ok=1 reasons=""

  w=$(mode_width "$mode"); h=$(mode_height "$mode"); f=$(mode_fps "$mode")
  floor=$(awk -v f="$f" 'BEGIN { printf "%.3f", 0.90 * f }')

  if [ "$PROVENANCE_COMPLETE" != yes ]; then
    record "RUN_SCORE: INCONCLUSIVE mode=$mode reason=provenance-incomplete aus=$RUN_AUS wall=${RUN_WALL_S}s stream_s=$RUN_STREAM_S fps=$RUN_FPS fps_source=$RUN_FPS_SOURCE"
    return 2
  fi
  if [ "$kind" = bounded ]; then
    [ "$RUN_AUS" -eq "$want_aus" ] || { ok=0; reasons="$reasons aus=$RUN_AUS/$want_aus"; }
  else
    # Sustained runs are time-bounded, so "delivered every requested AU" is
    # replaced by "survived the window and delivered something".
    [ "$RUN_AUS" -gt 0 ] || { ok=0; reasons="$reasons aus=0"; }
  fi
  if [ "$(awk -v a="$RUN_FPS" -v b="$floor" 'BEGIN { print (a >= b) ? 1 : 0 }')" != 1 ]; then
    ok=0; reasons="$reasons fps=$RUN_FPS<floor=$floor"
  fi
  if [ "$RUN_GEOM" != "${w}x${h}" ]; then
    ok=0; reasons="$reasons geometry=${RUN_GEOM:-unreadable}!=${w}x${h}"
  fi
  if [ "$RUN_ERRORS" -ne 0 ]; then
    ok=0; reasons="$reasons element_errors=$RUN_ERRORS"
  fi

  if [ -z "$RUN_GEOM" ] && [ "$RUN_AUS" -eq 0 ] && [ "$RUN_INVALID_MODE" -eq 0 ] && [ "$RUN_ERRORS" -eq 0 ]; then
    record "RUN_SCORE: INCONCLUSIVE mode=$mode reason=no-transcript-signal aus=$RUN_AUS wall=${RUN_WALL_S}s stream_s=$RUN_STREAM_S fps=$RUN_FPS fps_source=$RUN_FPS_SOURCE"
    return 2
  fi
  if [ "$ok" -eq 1 ]; then
    record "RUN_SCORE: PASS mode=$mode aus=$RUN_AUS wall=${RUN_WALL_S}s stream_s=$RUN_STREAM_S fps=$RUN_FPS fps_source=$RUN_FPS_SOURCE floor=$floor geometry=$RUN_GEOM errors=0"
    return 0
  fi
  record "RUN_SCORE: FAIL mode=$mode aus=$RUN_AUS wall=${RUN_WALL_S}s stream_s=$RUN_STREAM_S fps=$RUN_FPS fps_source=$RUN_FPS_SOURCE floor=$floor geometry=${RUN_GEOM:-unreadable} errors=$RUN_ERRORS invalid_mode=$RUN_INVALID_MODE reasons:$reasons"
  return 1
}

# --- operator prompts -------------------------------------------------------
#
# Returns 0 confirmed, 77 skipped (unattended), 1 aborted or no tty. A skip is
# NEVER a pass: callers record ARM_SKIPPED and degrade the drill verdict.

prompt_operator() {
  local text=$1 token=$2 arm=$3 replicate=$4
  local answer tries=0

  if [ "${CERALIVE_BOARD_UNATTENDED:-0}" = "1" ]; then
    record "ARM_SKIPPED: arm=$arm replicate=$replicate reason=unattended prompt='$text'"
    return 77
  fi
  if [ ! -r /dev/tty ]; then
    record "ARM_SKIPPED: arm=$arm replicate=$replicate reason=no-tty prompt='$text'"
    return 1
  fi
  while [ "$tries" -lt 3 ]; do
    tries=$((tries + 1))
    printf '\n>>> %s\n>>> type %s to confirm, or ABORT to stop: ' "$text" "$token" > /dev/tty
    IFS= read -r answer < /dev/tty || { record "ARM_ABORTED: arm=$arm replicate=$replicate reason=read-failed"; return 1; }
    case "$answer" in
      "$token") record "OPERATOR_CONFIRMED: arm=$arm replicate=$replicate token=$token at=$(ts_utc)"; return 0 ;;
      ABORT) record "ARM_ABORTED: arm=$arm replicate=$replicate reason=operator-abort"; return 1 ;;
      *) printf '>>> expected "%s"\n' "$token" > /dev/tty ;;
    esac
  done
  record "ARM_ABORTED: arm=$arm replicate=$replicate reason=no-confirmation-after-3-tries"
  return 1
}

# --- (a) TRANSITION matrix (Drills A and B) --------------------------------

transition_pairs_for_class() {
  case "$1" in
    increase) printf '%s\n' "1280x720@30>1920x1080@30" "1920x1080@30>3840x2160@30" ;;
    same)     printf '%s\n' "1920x1080@30>1920x1080@30" ;;
    decrease) printf '%s\n' "3840x2160@30>1920x1080@30" "1920x1080@30>1280x720@30" ;;
  esac
}

cmd_transition() {
  require_gate; require_env_name; require_root
  require_tools gst-launch-1.0 timeout sha256sum lsusb awk

  local n=${REPEAT:-$N_TRANSITION_CLASS}
  local -a classes=()
  case "$CLASSES" in
    all) classes=(increase same decrease) ;;
    increase|same|decrease) classes=("$CLASSES") ;;
    *) echo "FAIL: --class must be increase|same|decrease|all" >&2; exit 1 ;;
  esac

  open_rundir "transition"
  trap drill_cleanup EXIT INT TERM
  assert_verified_so
  emit_provenance "$RUNDIR"
  park_cerastream

  record "MODE: transition policy=$POLICY repeat=$n classes=${classes[*]} vid_pid=$VID_PID"

  local class pair from to i idx=0 fails total inconclusive commit_aus
  local -a class_pairs=()
  local increase_fails=0 increase_total=0
  local any_fail=0 any_inconclusive=0

  for class in "${classes[@]}"; do
    fails=0; total=0; inconclusive=0
    for i in $(seq 1 "$n"); do
      # Rotate through the class's prescribed pairs so a class with two pairs
      # samples both evenly rather than hammering the first one.
      mapfile -t class_pairs < <(transition_pairs_for_class "$class")
      pair=${class_pairs[$(( (i - 1) % ${#class_pairs[@]} ))]}
      from=${pair%%>*}; to=${pair##*>}
      idx=$((idx + 1))

      log "--- $class replicate $i/$n: commit $from then negotiate $to ---"
      gst_run "$RUNDIR/t$(printf '%03d' "$idx")-commit.log" "$from" "$COMMIT_BUFFERS"
      commit_aus=$RUN_AUS
      sleep 2
      gst_run "$RUNDIR/t$(printf '%03d' "$idx")-subject.log" "$to" "$SUBJECT_BUFFERS"
      total=$((total + 1))

      if [ "$RUN_INVALID_MODE" -gt 0 ]; then
        fails=$((fails + 1))
        record "TRANSITION: FAIL class=$class replicate=$i from=$from to=$to invalid_mode=$RUN_INVALID_MODE commit_aus=$commit_aus"
      elif [ "$RUN_AUS" -eq 0 ]; then
        inconclusive=$((inconclusive + 1))
        record "TRANSITION: INCONCLUSIVE class=$class replicate=$i from=$from to=$to reason=no-aus-no-error commit_aus=$commit_aus"
      else
        record "TRANSITION: PASS class=$class replicate=$i from=$from to=$to aus=$RUN_AUS fps=$RUN_FPS geometry=${RUN_GEOM:-unreadable} commit_aus=$commit_aus"
      fi
      sleep 2
    done

    record "CLASS_COUNTS: class=$class n=$total failures=$fails inconclusive=$inconclusive"
    if [ "$fails" -gt 0 ]; then
      record "VERDICT_CLASS: FAIL class=$class failures=$fails/$total"
      any_fail=1
    elif [ "$inconclusive" -gt 0 ]; then
      record "VERDICT_CLASS: INCONCLUSIVE class=$class inconclusive=$inconclusive/$total"
      any_inconclusive=1
    else
      record "VERDICT_CLASS: PASS class=$class failures=0/$total"
    fi
    if [ "$class" = increase ]; then
      increase_fails=$fails
      increase_total=$total
    fi
  done

  if [ "$increase_total" -gt 0 ]; then
    local rate branch
    rate=$(awk -v f="$increase_fails" -v t="$increase_total" 'BEGIN { printf "%.1f", (t > 0) ? 100.0 * f / t : 0 }')
    if [ "$increase_fails" -eq 0 ]; then branch=NOT_REPRODUCED
    elif [ "$(awk -v r="$rate" 'BEGIN { print (r >= 30.0) ? 1 : 0 }')" = 1 ]; then branch=REPRODUCED
    else branch=AMBIGUOUS
    fi
    record "RULE_A_BRANCH: $branch policy=$POLICY n=$increase_total failures=$increase_fails rate=${rate}%"
    if [ "$POLICY" = single ] && [ "$branch" != REPRODUCED ]; then
      # The conservative branch still has to hand every downstream todo its
      # machine-final lines, or they have nothing to key on.
      record "RULE_G: G3 provenance=status-quo"
      record "RULE_C_FINAL: C2 rate=$STATUS_QUO_RATE provenance=status-quo-no-rule-c"
    fi
  else
    record "RULE_A_BRANCH: NOT_RUN reason=increase-class-not-selected"
  fi

  if [ "$PROVENANCE_COMPLETE" != yes ]; then finish "INCONCLUSIVE"; fi
  if [ "$any_fail" -eq 1 ]; then finish "FAIL"; fi
  if [ "$any_inconclusive" -eq 1 ]; then finish "INCONCLUSIVE"; fi
  finish "PASS"
}

# --- (b) PHANTOM matrix + (d) REPLUG / COLD-START arms ---------------------

cmd_phantom() {
  require_gate; require_env_name; require_root
  require_tools gst-launch-1.0 timeout sha256sum lsusb awk

  [ "${#MODES[@]}" -gt 0 ] || { echo "FAIL: phantom needs at least one --mode <WxH@fps>" >&2; exit 1; }
  local m
  for m in "${MODES[@]}"; do
    mode_valid "$m" || { echo "FAIL: bad --mode '$m' (want WxH@fps)" >&2; exit 1; }
  done
  mode_valid "$SMALLER_MODE" || { echo "FAIL: bad --smaller-mode '$SMALLER_MODE'" >&2; exit 1; }

  local n
  case "$ARM" in
    after-smaller) n=${REPEAT:-$N_AFTER_SMALLER} ;;
    cold-start)    n=${REPEAT:-$N_COLD_START} ;;
    after-replug)  n=${REPEAT:-$N_AFTER_REPLUG} ;;
    *) echo "FAIL: --arm must be after-smaller|cold-start|after-replug" >&2; exit 1 ;;
  esac

  open_rundir "phantom-$ARM"
  trap drill_cleanup EXIT INT TERM
  assert_verified_so
  emit_provenance "$RUNDIR"
  park_cerastream

  record "MODE: phantom arm=$ARM policy=$POLICY repeat=$n buffers=$BUFFERS modes=${MODES[*]} smaller=$SMALLER_MODE vid_pid=$VID_PID"

  local i idx=0 rc mode passes fails incs skips
  local any_fail=0 any_inconclusive=0 any_skip=0

  for mode in "${MODES[@]}"; do
    passes=0; fails=0; incs=0; skips=0
    for i in $(seq 1 "$n"); do
      idx=$((idx + 1))
      case "$ARM" in
        after-smaller)
          log "--- $mode replicate $i/$n: commit $SMALLER_MODE first ---"
          gst_run "$RUNDIR/p$(printf '%03d' "$idx")-commit.log" "$SMALLER_MODE" "$COMMIT_BUFFERS"
          sleep 2
          ;;
        cold-start)
          prompt_operator "POWER-CYCLE the camera now (off, wait, on, wait for enumeration)" \
            POWERCYCLED "$ARM" "$i"
          rc=$?
          if [ "$rc" -eq 77 ]; then skips=$((skips + 1)); any_skip=1; continue; fi
          if [ "$rc" -ne 0 ]; then incs=$((incs + 1)); any_inconclusive=1; continue; fi
          sleep 3
          ;;
        after-replug)
          prompt_operator "PHYSICALLY REPLUG the camera now (unplug, wait, plug back in)" \
            REPLUGGED "$ARM" "$i"
          rc=$?
          if [ "$rc" -eq 77 ]; then skips=$((skips + 1)); any_skip=1; continue; fi
          if [ "$rc" -ne 0 ]; then incs=$((incs + 1)); any_inconclusive=1; continue; fi
          sleep 3
          ;;
      esac

      log "--- $mode replicate $i/$n: bounded run ($BUFFERS AUs) ---"
      gst_run "$RUNDIR/p$(printf '%03d' "$idx")-subject.log" "$mode" "$BUFFERS"
      score_run "$mode" "$BUFFERS" bounded
      rc=$?
      local cell_result
      case "$rc" in
        0) passes=$((passes + 1)); cell_result=PASS ;;
        1) fails=$((fails + 1)); any_fail=1; cell_result=FAIL ;;
        *) incs=$((incs + 1)); any_inconclusive=1; cell_result=INCONCLUSIVE ;;
      esac
      record "CELL_RUN: arm=$ARM mode=$mode replicate=$i result=$cell_result"
      sleep 2
    done

    record "CELL_COUNTS: arm=$ARM mode=$mode n=$n passes=$passes failures=$fails inconclusive=$incs skipped=$skips"
    if [ "$skips" -gt 0 ]; then
      record "VERDICT_CELL: SKIPPED arm=$ARM mode=$mode skipped=$skips/$n reason=unattended"
    elif [ "$fails" -gt 0 ]; then
      record "VERDICT_CELL: FAIL arm=$ARM mode=$mode failures=$fails/$n"
    elif [ "$incs" -gt 0 ]; then
      record "VERDICT_CELL: INCONCLUSIVE arm=$ARM mode=$mode inconclusive=$incs/$n"
    else
      record "VERDICT_CELL: PASS arm=$ARM mode=$mode passes=$passes/$n"
    fi
  done

  if [ "$PROVENANCE_COMPLETE" != yes ]; then finish "INCONCLUSIVE"; fi
  if [ "$any_fail" -eq 1 ]; then finish "FAIL"; fi
  # A skipped arm can never be a PASS: C1 is unreachable from here and the
  # aggregate must be able to see that.
  if [ "$any_skip" -eq 1 ] || [ "$any_inconclusive" -eq 1 ]; then finish "INCONCLUSIVE"; fi
  finish "PASS"
}

# --- (c) SUSTAINED mode -----------------------------------------------------

sample_thermal_into() {
  local file=$1 interval=$2 pid_file=$3
  (
    while :; do
      printf '%s %s\n' "$(ts_utc)" "$(thermal_snapshot)" >> "$file"
      sleep "$interval"
    done
  ) &
  echo $! > "$pid_file"
}

cmd_sustained() {
  require_gate; require_env_name; require_root
  require_tools gst-launch-1.0 timeout sha256sum lsusb awk

  [ "${#MODES[@]}" -gt 0 ] || { echo "FAIL: sustained needs --mode <WxH@fps>" >&2; exit 1; }
  mode_valid "${MODES[0]}" || { echo "FAIL: bad --mode '${MODES[0]}'" >&2; exit 1; }
  if [ "$MINUTES" -lt 3 ] || [ "$MINUTES" -gt 5 ]; then
    echo "FAIL: --minutes must be 3..5 (the pre-registered sustained window)" >&2
    exit 1
  fi

  local n=${REPEAT:-3} mode=${MODES[0]} secs=$((MINUTES * 60))

  open_rundir "sustained"
  trap drill_cleanup EXIT INT TERM
  assert_verified_so
  emit_provenance "$RUNDIR"
  park_cerastream

  record "MODE: sustained policy=$POLICY mode=$mode minutes=$MINUTES repeat=$n warm_claimed=$WARM"

  if [ "$WARM" = yes ]; then
    prompt_operator "confirm the board/camera are already WARM (a prior run just ended)" \
      WARM sustained-warm 0
    case $? in
      0) record "SUSTAINED_WARM: confirmed" ;;
      77) record "SUSTAINED_WARM: unverified reason=unattended (recorded, not assumed)" ;;
      *) record "SUSTAINED_WARM: unverified reason=aborted-or-no-tty" ;;
    esac
  fi

  local i rc passes=0 fails=0 incs=0 tpid_file tpid
  for i in $(seq 1 "$n"); do
    log "--- sustained replicate $i/$n: $mode for ${MINUTES}m ---"
    tpid_file="$RUNDIR/.thermal-$i.pid"
    sample_thermal_into "$RUNDIR/thermal-$(printf '%02d' "$i").log" "$THERMAL_INTERVAL" "$tpid_file"
    gst_run "$RUNDIR/s$(printf '%02d' "$i").log" "$mode" 0 "$secs"
    tpid=$(cat "$tpid_file" 2>/dev/null)
    [ -n "$tpid" ] && kill "$tpid" 2>/dev/null
    rm -f "$tpid_file"

    score_run "$mode" 0 sustained
    rc=$?
    case "$rc" in
      0) passes=$((passes + 1)); record "SUSTAINED_RUN: PASS replicate=$i aus=$RUN_AUS wall=${RUN_WALL_S}s stream_s=$RUN_STREAM_S fps=$RUN_FPS fps_source=$RUN_FPS_SOURCE" ;;
      1) fails=$((fails + 1)); record "SUSTAINED_RUN: FAIL replicate=$i aus=$RUN_AUS wall=${RUN_WALL_S}s stream_s=$RUN_STREAM_S fps=$RUN_FPS fps_source=$RUN_FPS_SOURCE" ;;
      *) incs=$((incs + 1)); record "SUSTAINED_RUN: INCONCLUSIVE replicate=$i aus=$RUN_AUS wall=${RUN_WALL_S}s stream_s=$RUN_STREAM_S fps=$RUN_FPS fps_source=$RUN_FPS_SOURCE" ;;
    esac
    sleep 5
  done

  record "SUSTAINED_COUNTS: mode=$mode n=$n passes=$passes failures=$fails inconclusive=$incs thermal_end=$(thermal_snapshot)"

  if [ "$PROVENANCE_COMPLETE" != yes ]; then finish "INCONCLUSIVE"; fi
  if [ "$fails" -gt 0 ]; then finish "FAIL"; fi
  if [ "$incs" -gt 0 ]; then finish "INCONCLUSIVE"; fi
  finish "PASS"
}

# --- verdict aggregation (Rule G, Rule C) ----------------------------------
#
# The campaign verdicts are computed HERE for the same reason the per-run ones
# are: a human reading counts out of a log is exactly the failure mode the
# pre-registered decision rules exist to prevent.

run_is_sealed() { [ -f "$1/RUN_COMPLETE" ]; }

run_summary() { printf '%s' "$1/summary.txt"; }

cmd_verdict_rule_g() {
  local dir s policy classes_ok=1 total_fail=0 retry_runs=0 short_class=0
  local line class n fails

  for dir in "${RUN_DIRS[@]}"; do
    s=$(run_summary "$dir")
    if ! run_is_sealed "$dir"; then
      echo "RUN_UNSEALED: $dir (no RUN_COMPLETE - INCONCLUSIVE by definition)"
      classes_ok=0
      continue
    fi
    policy=$(awk -F'policy=' '/^MODE: transition/ { split($2, a, " "); print a[1]; exit }' "$s" 2>/dev/null)
    [ "$policy" = retry ] || { echo "RUN_SKIPPED: $dir policy=${policy:-unknown} (Rule G scores the retry arm)"; continue; }
    retry_runs=$((retry_runs + 1))
    while IFS= read -r line; do
      class=$(printf '%s' "$line" | grep -o 'class=[^ ]*' | head -1 | cut -d= -f2)
      n=$(printf '%s' "$line" | grep -o ' n=[0-9]*' | head -1 | grep -o '[0-9]*')
      fails=$(printf '%s' "$line" | grep -o 'failures=[0-9]*' | head -1 | grep -o '[0-9]*')
      n=${n:-0}; fails=${fails:-0}
      echo "RULE_G_INPUT: run=$dir class=${class:-?} n=$n failures=$fails"
      [ "$fails" -eq 0 ] || total_fail=$((total_fail + fails))
      [ "$n" -ge "$N_TRANSITION_CLASS" ] || short_class=$((short_class + 1))
    done <<< "$(grep '^CLASS_COUNTS:' "$s" 2>/dev/null)"
  done

  if [ "$retry_runs" -eq 0 ] || [ "$classes_ok" -ne 1 ] || [ "$short_class" -ne 0 ] || [ "$total_fail" -ne 0 ]; then
    echo "RULE_G: G3 retry_runs=$retry_runs failures=$total_fail undersized_classes=$short_class sealed=$classes_ok"
    echo "VERDICT: PASS rule=g result=G3"
    return 0
  fi
  echo "RULE_G: G1 retry_runs=$retry_runs failures=0 undersized_classes=0"
  echo "VERDICT: PASS rule=g result=G1"
  return 0
}

# Highest CONTIGUOUS ceiling: walk the advertised 4K ladder upward and stop
# below the FIRST mode that did not pass. A ceiling that re-admits a failed
# lower mode is unreachable by construction.
contiguous_ceiling() {
  local passed_list=$1 i mode rate ceiling=$STATUS_QUO_RATE
  for i in "${!CANDIDATE_MODES[@]}"; do
    mode=${CANDIDATE_MODES[$i]}
    rate=${CANDIDATE_RATES[$i]}
    case " $passed_list " in
      *" $mode "*) ceiling=$rate ;;
      *) break ;;
    esac
  done
  printf '%s' "$ceiling"
}

cmd_verdict_rule_c() {
  local dir s line arm mode result n fails
  local passed_list="" failed_any=0 skipped_any=0 inconclusive_any=0 unsealed=0
  local -A cell_state=()
  local authorizing=(3840x2160@60 3840x2160@50 3840x2160@48)
  local arms=(after-smaller cold-start after-replug)

  for dir in "${RUN_DIRS[@]}"; do
    s=$(run_summary "$dir")
    if ! run_is_sealed "$dir"; then
      echo "RUN_UNSEALED: $dir (no RUN_COMPLETE - INCONCLUSIVE by definition)"
      unsealed=$((unsealed + 1))
      continue
    fi
    while IFS= read -r line; do
      result=$(printf '%s' "$line" | awk '{print $2}')
      arm=$(printf '%s' "$line" | grep -o 'arm=[^ ]*' | head -1 | cut -d= -f2)
      mode=$(printf '%s' "$line" | grep -o 'mode=[^ ]*' | head -1 | cut -d= -f2)
      if [ -z "$arm" ] || [ -z "$mode" ]; then continue; fi
      cell_state["$arm/$mode"]=$result
      echo "RULE_C_INPUT: run=$dir arm=$arm mode=$mode result=$result"
      case "$result" in
        FAIL) failed_any=1 ;;
        SKIPPED) skipped_any=1 ;;
        INCONCLUSIVE) inconclusive_any=1 ;;
      esac
    done <<< "$(grep '^VERDICT_CELL:' "$s" 2>/dev/null)"
    while IFS= read -r line; do
      arm=$(printf '%s' "$line" | grep -o 'arm=[^ ]*' | head -1 | cut -d= -f2)
      mode=$(printf '%s' "$line" | grep -o 'mode=[^ ]*' | head -1 | cut -d= -f2)
      n=$(printf '%s' "$line" | grep -o ' n=[0-9]*' | head -1 | grep -o '[0-9]*')
      fails=$(printf '%s' "$line" | grep -o 'failures=[0-9]*' | head -1 | grep -o '[0-9]*')
      echo "RULE_C_COUNTS: run=$dir arm=${arm:-?} mode=${mode:-?} n=${n:-0} failures=${fails:-0}"
    done <<< "$(grep '^CELL_COUNTS:' "$s" 2>/dev/null)"
  done

  # A 4K mode counts as passed only when EVERY arm that ran for it passed and
  # no arm for it was skipped or inconclusive.
  local m a state mode_ok
  for m in "${CANDIDATE_MODES[@]}"; do
    mode_ok=0
    for a in "${arms[@]}"; do
      state=${cell_state["$a/$m"]:-}
      [ -n "$state" ] || continue
      if [ "$state" = PASS ]; then mode_ok=1; else mode_ok=0; break; fi
    done
    [ "$mode_ok" -eq 1 ] && passed_list="$passed_list $m"
  done
  echo "RULE_C_PASSED_MODES:${passed_list:- none}"

  # Sustained requirement: at least 3 sealed, PASSing sustained runs.
  local sustained_pass=0
  for dir in "${SUSTAINED_RUN_DIRS[@]}"; do
    run_is_sealed "$dir" || { echo "RUN_UNSEALED: $dir"; continue; }
    if grep -q '^VERDICT: PASS' "$(run_summary "$dir")" 2>/dev/null; then
      sustained_pass=$((sustained_pass + 1))
    fi
  done
  echo "RULE_C_SUSTAINED: passing=$sustained_pass required=3"

  # Vendor gate: a C1 stays PROVISIONAL until the vendor-kernel session passes.
  local vendor_runs=0 vendor_pass=0
  for dir in "${VENDOR_RUN_DIRS[@]}"; do
    vendor_runs=$((vendor_runs + 1))
    run_is_sealed "$dir" || { echo "RUN_UNSEALED: $dir"; continue; }
    if grep -q '^VERDICT: PASS' "$(run_summary "$dir")" 2>/dev/null; then
      vendor_pass=$((vendor_pass + 1))
    fi
  done
  echo "RULE_C_VENDOR_GATE: runs=$vendor_runs passing=$vendor_pass"

  local all_authorizing_pass=1
  for m in "${authorizing[@]}"; do
    for a in "${arms[@]}"; do
      state=${cell_state["$a/$m"]:-MISSING}
      [ "$state" = PASS ] || { all_authorizing_pass=0; echo "RULE_C_GAP: arm=$a mode=$m state=$state"; }
    done
  done

  local aggregate ceiling
  if [ "$all_authorizing_pass" -eq 1 ] && [ "$failed_any" -eq 0 ] && [ "$skipped_any" -eq 0 ] &&
     [ "$inconclusive_any" -eq 0 ] && [ "$unsealed" -eq 0 ] && [ "$sustained_pass" -ge 3 ]; then
    aggregate=C1
    ceiling=0
  else
    aggregate=C2
    ceiling=$(contiguous_ceiling "$passed_list")
  fi
  echo "RULE_C_AGGREGATE: $aggregate rate=$ceiling"

  if [ "$aggregate" = C1 ]; then
    if [ "$vendor_runs" -gt 0 ] && [ "$vendor_pass" -eq "$vendor_runs" ]; then
      echo "RULE_C_FINAL: C1 rate=0 provenance=vendor-gate-passed"
    else
      ceiling=$(contiguous_ceiling "$passed_list")
      echo "RULE_C_FINAL: C2 rate=$ceiling provenance=c1-provisional-vendor-gate-not-passed"
    fi
  else
    echo "RULE_C_FINAL: C2 rate=$ceiling provenance=edge-aggregate-contiguous-ceiling"
  fi
  echo "VERDICT: PASS rule=c result=$aggregate"
  return 0
}

cmd_verdict() {
  require_gate
  [ "${#RUN_DIRS[@]}" -gt 0 ] || { echo "FAIL: verdict needs at least one --run <dir>" >&2; exit 1; }
  case "$RULE" in
    g) cmd_verdict_rule_g; exit $? ;;
    c) cmd_verdict_rule_c; exit $? ;;
    *) echo "FAIL: --rule must be g or c" >&2; exit 1 ;;
  esac
}

# --- argument parsing -------------------------------------------------------

main() {
[ $# -gt 0 ] || { usage; exit 1; }

SUBCOMMAND=$1; shift
case "$SUBCOMMAND" in
  -h|--help|help) usage; exit 0 ;;
esac

while [ $# -gt 0 ]; do
  case "$1" in
    --env) ENVNAME=$2; shift 2 ;;
    --so) SO_SRC=$2; shift 2 ;;
    --target) TARGET_SO=$2; shift 2 ;;
    --backup-root) BACKUP_ROOT=$2; shift 2 ;;
    --policy) POLICY=$2; shift 2 ;;
    --repeat) REPEAT=$2; shift 2 ;;
    --class) CLASSES=$2; shift 2 ;;
    --arm) ARM=$2; shift 2 ;;
    --mode) MODES+=("$2"); shift 2 ;;
    --smaller-mode) SMALLER_MODE=$2; shift 2 ;;
    --buffers) BUFFERS=$2; shift 2 ;;
    --commit-buffers) COMMIT_BUFFERS=$2; shift 2 ;;
    --subject-buffers) SUBJECT_BUFFERS=$2; shift 2 ;;
    --minutes) MINUTES=$2; shift 2 ;;
    --thermal-interval) THERMAL_INTERVAL=$2; shift 2 ;;
    --warm) WARM=yes; shift ;;
    --vid-pid) VID_PID=$2; shift 2 ;;
    --outdir) OUTDIR=$2; shift 2 ;;
    --rule) RULE=$2; shift 2 ;;
    --run) RUN_DIRS+=("$2"); shift 2 ;;
    --vendor-run) VENDOR_RUN_DIRS+=("$2"); shift 2 ;;
    --sustained-run) SUSTAINED_RUN_DIRS+=("$2"); shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 1 ;;
  esac
done

case "$POLICY" in
  single|double|retry|default) : ;;
  *) echo "FAIL: --policy must be single|double|retry|default" >&2; exit 1 ;;
esac

case "$SUBCOMMAND" in
  session-start)   cmd_session_start ;;
  session-end)     cmd_session_end ;;
  session-recover) cmd_session_recover ;;
  session-status)  cmd_session_status ;;
  transition)      cmd_transition ;;
  phantom)         cmd_phantom ;;
  replug)          ARM=after-replug; cmd_phantom ;;
  cold-start)      ARM=cold-start; cmd_phantom ;;
  sustained)       cmd_sustained ;;
  verdict)         cmd_verdict ;;
  *) echo "unknown subcommand: $SUBCOMMAND" >&2; usage; exit 1 ;;
esac
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  main "$@"
fi
