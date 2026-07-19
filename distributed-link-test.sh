#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"
BUILD_DIR="${REPO_ROOT}/build"
REMOTE_HOST="brat-44"
REMOTE_DIR=""
TEST_DURATION=25
STARTUP_DELAY=6
LOG_ROOT=""
SKIP_BUILD=0
SKIP_COPY=0

ARTIFACTS=(
  "instrument-cli"
  "libsensor-lib.so"
  "libsensor-lib.a"
  "link-callback-test"
  "repeat-sensing-test"
)

ARTIFACT_PATHS=()

usage() {
  cat <<'EOF'
Usage: ./distributed-link-test.sh [options] [remote-host]

Build sensor-lib, copy the fresh test artifacts to the remote host, then run the
distributed Wi-Fi link-callback harness in both directions:
1. local host as hotspot, remote host as client
2. remote host as hotspot, local host as client

Options:
  --build-dir DIR       Build directory to use (default: ./build)
  --remote-dir DIR      Remote directory for copied artifacts (default: remote $HOME)
  --duration SEC        Timeout for each side of each test run (default: 25)
  --startup-delay SEC   Delay after starting the hotspot side before client start (default: 6)
  --log-root DIR        Directory for run logs (default: build/distributed-link-logs/<timestamp>)
  --skip-build          Reuse existing build artifacts
  --skip-copy           Do not scp artifacts before testing
  -h, --help            Show this help

Examples:
  ./distributed-link-test.sh
  ./distributed-link-test.sh brat-44
  ./distributed-link-test.sh --duration 35 --startup-delay 8 other-host
EOF
}

require_cmd() {
  local cmd="$1"
  command -v "$cmd" >/dev/null 2>&1 || {
    echo "Missing required command: $cmd" >&2
    exit 1
  }
}

quote_join() {
  local out=""
  local item
  for item in "$@"; do
    out+=" $(printf '%q' "$item")"
  done
  printf '%s' "${out# }"
}

cleanup_pids=()
LAST_PID=""
LAST_WAIT_RC=0
cleanup() {
  local pid
  for pid in "${cleanup_pids[@]:-}"; do
    [[ -n "$pid" ]] || continue
    if kill -0 "$pid" >/dev/null 2>&1; then
      kill "$pid" >/dev/null 2>&1 || true
      wait "$pid" >/dev/null 2>&1 || true
    fi
  done
}
trap cleanup EXIT

while (($# > 0)); do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --remote-dir)
      REMOTE_DIR="$2"
      shift 2
      ;;
    --duration)
      TEST_DURATION="$2"
      shift 2
      ;;
    --startup-delay)
      STARTUP_DELAY="$2"
      shift 2
      ;;
    --log-root)
      LOG_ROOT="$2"
      shift 2
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    --skip-copy)
      SKIP_COPY=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
    *)
      REMOTE_HOST="$1"
      shift
      ;;
  esac
done

require_cmd cmake
require_cmd ssh
require_cmd scp
require_cmd timeout
require_cmd stdbuf

if [[ -z "$LOG_ROOT" ]]; then
  LOG_ROOT="${BUILD_DIR}/distributed-link-logs/$(date +%Y%m%d-%H%M%S)"
fi
mkdir -p "$LOG_ROOT"

BUILD_DIR="$(cd "$(dirname "$BUILD_DIR")" && pwd)/$(basename "$BUILD_DIR")"
mkdir -p "$BUILD_DIR"

if [[ -z "$REMOTE_DIR" ]]; then
  REMOTE_DIR="$(ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" 'printf %s "$HOME"')"
fi

LOCAL_HOSTNAME="$(hostname)"
REMOTE_HOSTNAME="$(ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" 'hostname')"

echo "Local host:  $LOCAL_HOSTNAME"
echo "Remote host: $REMOTE_HOSTNAME ($REMOTE_HOST)"
echo "Build dir:   $BUILD_DIR"
echo "Remote dir:  $REMOTE_DIR"
echo "Log root:    $LOG_ROOT"

if ((SKIP_BUILD == 0)); then
  echo
  echo "== Building =="
  cmake -S "$REPO_ROOT" -B "$BUILD_DIR"
  cmake --build "$BUILD_DIR" -j
fi

for artifact in "${ARTIFACTS[@]}"; do
  if [[ ! -f "$BUILD_DIR/$artifact" ]]; then
    echo "Missing expected artifact: $BUILD_DIR/$artifact" >&2
    exit 1
  fi
  ARTIFACT_PATHS+=("$BUILD_DIR/$artifact")
done

if ((SKIP_COPY == 0)); then
  echo
  echo "== Copying artifacts to $REMOTE_HOST:$REMOTE_DIR =="
  ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" "mkdir -p $(printf '%q' "$REMOTE_DIR")"
  scp -o BatchMode=yes -o ConnectTimeout=8 \
    "${ARTIFACT_PATHS[@]}" \
    "$REMOTE_HOST:$REMOTE_DIR/"
fi

run_local_harness() {
  local wifi_mode="$1"
  local logfile="$2"
  shift 2

  (
    cd "$BUILD_DIR"
    export LD_LIBRARY_PATH="$BUILD_DIR:${LD_LIBRARY_PATH:-}"
    exec stdbuf -oL -eL timeout -s INT "${TEST_DURATION}s" \
      ./instrument-cli link-callback --wifi-mode "$wifi_mode"
  ) >"$logfile" 2>&1 &
  LAST_PID="$!"
  cleanup_pids+=("$LAST_PID")
}

run_remote_harness() {
  local wifi_mode="$1"
  local logfile="$2"

  local remote_cmd
  remote_cmd=$(
    cat <<EOF
cd $(printf '%q' "$REMOTE_DIR")
export LD_LIBRARY_PATH=$(printf '%q' "$REMOTE_DIR"):\${LD_LIBRARY_PATH:-}
exec stdbuf -oL -eL timeout -s INT ${TEST_DURATION}s ./instrument-cli link-callback --wifi-mode $(printf '%q' "$wifi_mode")
EOF
  )

  ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" "bash -lc $(printf '%q' "$remote_cmd")" \
    >"$logfile" 2>&1 &
  LAST_PID="$!"
  cleanup_pids+=("$LAST_PID")
}

wait_and_capture_rc() {
  local pid="$1"
  [[ -n "$pid" ]] || {
    LAST_WAIT_RC=1
    return
  }
  set +e
  wait "$pid"
  LAST_WAIT_RC=$?
  set -e
}

show_log() {
  local label="$1"
  local logfile="$2"
  echo "--- $label ---"
  if [[ -s "$logfile" ]]; then
    cat "$logfile"
  else
    echo "<no output>"
  fi
}

run_direction() {
  local name="$1"
  local hotspot_side="$2"

  local hotspot_log="${LOG_ROOT}/${name}.hotspot.log"
  local client_log="${LOG_ROOT}/${name}.client.log"
  local hotspot_pid client_pid hotspot_rc client_rc

  echo
  echo "== $name =="
  echo "Hotspot side: $hotspot_side"

  if [[ "$hotspot_side" == "local" ]]; then
    run_local_harness hotspot "$hotspot_log"
    hotspot_pid="$LAST_PID"
    sleep "$STARTUP_DELAY"
    run_remote_harness client "$client_log"
    client_pid="$LAST_PID"
  else
    run_remote_harness hotspot "$hotspot_log"
    hotspot_pid="$LAST_PID"
    sleep "$STARTUP_DELAY"
    run_local_harness client "$client_log"
    client_pid="$LAST_PID"
  fi

  wait_and_capture_rc "$client_pid"
  client_rc="$LAST_WAIT_RC"
  wait_and_capture_rc "$hotspot_pid"
  hotspot_rc="$LAST_WAIT_RC"

  cleanup_pids=()

  echo "Exit codes: hotspot=$hotspot_rc client=$client_rc"
  show_log "$name hotspot output" "$hotspot_log"
  show_log "$name client output" "$client_log"

  if [[ "$hotspot_rc" != "0" && "$hotspot_rc" != "124" && "$hotspot_rc" != "130" ]]; then
    return 1
  fi
  if [[ "$client_rc" != "0" && "$client_rc" != "124" && "$client_rc" != "130" ]]; then
    return 1
  fi
}

run_direction "local-hotspot_remote-client" "local"
run_direction "remote-hotspot_local-client" "remote"

echo
echo "Logs saved under: $LOG_ROOT"
