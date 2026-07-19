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
TEST_SSID="sensor-lib-link-callback"
TEST_PASSWORD="sensorlib123"
REMOTE_RECONNECT_TIMEOUT=90
RUN_ID="dltest-$(date +%Y%m%d-%H%M%S)-$$"

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
  --ssid NAME           Hotspot SSID used by the harness (default: sensor-lib-link-callback)
  --password PASS       Hotspot password used by the harness (default: sensorlib123)
  --remote-reconnect SEC
                        Seconds to wait for ssh to the remote host to recover (default: 90)
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
    --ssid)
      TEST_SSID="$2"
      shift 2
      ;;
    --password)
      TEST_PASSWORD="$2"
      shift 2
      ;;
    --remote-reconnect)
      REMOTE_RECONNECT_TIMEOUT="$2"
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
echo "Test SSID:   $TEST_SSID"

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
    export SENSOR_LIB_TEST_SSID="$TEST_SSID"
    export SENSOR_LIB_TEST_PASSWORD="$TEST_PASSWORD"
    exec stdbuf -oL -eL timeout -s INT "${TEST_DURATION}s" \
      ./instrument-cli link-callback --wifi-mode "$wifi_mode"
  ) >"$logfile" 2>&1 &
  LAST_PID="$!"
  cleanup_pids+=("$LAST_PID")
}

remote_meta_base() {
  printf '%s/.sensor-lib-distributed/%s' "$REMOTE_DIR" "$RUN_ID"
}

wait_for_remote_ssh() {
  local deadline=$((SECONDS + REMOTE_RECONNECT_TIMEOUT))
  while ((SECONDS < deadline)); do
    if ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" "true" >/dev/null 2>&1; then
      return 0
    fi
    sleep 3
  done
  return 1
}

run_remote_harness_detached() {
  local wifi_mode="$1"
  local logfile="$2"
  local remote_label="$3"
  local remote_base remote_log remote_status remote_cmd start_cmd

  remote_base="$(remote_meta_base)"
  remote_log="${remote_base}/${remote_label}.log"
  remote_status="${remote_base}/${remote_label}.status"

  remote_cmd=$(
    cat <<EOF
cd $(printf '%q' "$REMOTE_DIR")
export LD_LIBRARY_PATH=$(printf '%q' "$REMOTE_DIR"):\${LD_LIBRARY_PATH:-}
export SENSOR_LIB_TEST_SSID=$(printf '%q' "$TEST_SSID")
export SENSOR_LIB_TEST_PASSWORD=$(printf '%q' "$TEST_PASSWORD")
stdbuf -oL -eL timeout -s INT ${TEST_DURATION}s ./instrument-cli link-callback --wifi-mode $(printf '%q' "$wifi_mode")
rc=\$?
printf '%s\n' "\$rc" > $(printf '%q' "$remote_status")
exit "\$rc"
EOF
  )

  start_cmd=$(
    cat <<EOF
mkdir -p $(printf '%q' "$remote_base")
rm -f $(printf '%q' "$remote_log") $(printf '%q' "$remote_status")
nohup bash -lc $(printf '%q' "$remote_cmd") > $(printf '%q' "$remote_log") 2>&1 < /dev/null &
printf '%s\n' "\$!"
EOF
  )

  LAST_PID="$(ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" "bash -lc $(printf '%q' "$start_cmd")" | tail -n 1)"
  : >"$logfile"
}

collect_remote_log() {
  local remote_label="$1"
  local logfile="$2"
  local remote_base remote_log
  remote_base="$(remote_meta_base)"
  remote_log="${remote_base}/${remote_label}.log"

  ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" "cat $(printf '%q' "$remote_log")" >"$logfile" 2>&1 || true
}

read_remote_status() {
  local remote_label="$1"
  local remote_base remote_status
  remote_base="$(remote_meta_base)"
  remote_status="${remote_base}/${remote_label}.status"
  ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" "cat $(printf '%q' "$remote_status")" 2>/dev/null | tail -n 1
}

wait_for_remote_status() {
  local remote_label="$1"
  local deadline=$((SECONDS + REMOTE_RECONNECT_TIMEOUT))
  local status=""

  while ((SECONDS < deadline)); do
    if wait_for_remote_ssh; then
      status="$(read_remote_status "$remote_label")"
      if [[ -n "$status" ]]; then
        printf '%s' "$status"
        return 0
      fi
    fi
    sleep 3
  done
  return 1
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
  local remote_hotspot_label="${name}.hotspot"
  local remote_client_label="${name}.client"

  echo
  echo "== $name =="
  echo "Hotspot side: $hotspot_side"

  if [[ "$hotspot_side" == "local" ]]; then
    run_local_harness hotspot "$hotspot_log"
    hotspot_pid="$LAST_PID"
    sleep "$STARTUP_DELAY"
    run_remote_harness_detached client "$client_log" "$remote_client_label"
    client_pid="$LAST_PID"
  else
    run_remote_harness_detached hotspot "$hotspot_log" "$remote_hotspot_label"
    hotspot_pid="$LAST_PID"
    sleep "$STARTUP_DELAY"
    run_local_harness client "$client_log"
    client_pid="$LAST_PID"
  fi

  if [[ "$hotspot_side" == "local" ]]; then
    wait_and_capture_rc "$hotspot_pid"
    hotspot_rc="$LAST_WAIT_RC"
    if client_rc="$(wait_for_remote_status "$remote_client_label")"; then
      collect_remote_log "$remote_client_label" "$client_log"
    else
      echo "<remote client log/status did not recover within ${REMOTE_RECONNECT_TIMEOUT}s>" >>"$client_log"
      client_rc="ssh-reconnect-timeout"
    fi
  else
    wait_and_capture_rc "$client_pid"
    client_rc="$LAST_WAIT_RC"
    if hotspot_rc="$(wait_for_remote_status "$remote_hotspot_label")"; then
      collect_remote_log "$remote_hotspot_label" "$hotspot_log"
    else
      echo "<remote hotspot log/status did not recover within ${REMOTE_RECONNECT_TIMEOUT}s>" >>"$hotspot_log"
      hotspot_rc="ssh-reconnect-timeout"
    fi
  fi

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
