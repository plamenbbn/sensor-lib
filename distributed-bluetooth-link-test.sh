#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"
BUILD_DIR="${REPO_ROOT}/build"
REMOTE_HOST="brat-44"
REMOTE_DIR=""
TEST_DURATION=45
LOG_ROOT=""
SKIP_BUILD=0
SKIP_COPY=0
REMOTE_RECONNECT_TIMEOUT=90
RUN_ID="dbttest-$(date +%Y%m%d-%H%M%S)-$$"
BT_PRIME=1

ARTIFACTS=(
  "instrument-cli"
  "libsensor-lib.so"
  "libsensor-lib.a"
  "link-callback-test"
  "repeat-sensing-test"
)

ARTIFACT_PATHS=()
cleanup_pids=()
LAST_PID=""
LAST_WAIT_RC=0

usage() {
  cat <<'EOF'
Usage: ./distributed-bluetooth-link-test.sh [options] [remote-host]

Build sensor-lib, copy fresh artifacts to the remote host, prepare both hosts for
Bluetooth auto-connect/auto-accept, then run a BT-only distributed
instrument-cli link-callback test on both sides at once.

Options:
  --build-dir DIR         Build directory to use (default: ./build)
  --remote-dir DIR        Remote directory for copied artifacts (default: remote $HOME)
  --duration SEC          Timeout for each side of the test run (default: 45)
  --log-root DIR          Directory for run logs (default: build/distributed-link-logs/<timestamp>-bt)
  --remote-reconnect SEC  Seconds to wait for ssh recovery (default: 90)
  --no-bt-prime           Skip the Bluetooth inquiry/l2ping priming step
  --skip-build            Reuse existing build artifacts
  --skip-copy             Do not scp artifacts before testing
  -h, --help              Show this help
EOF
}

require_cmd() {
  local cmd="$1"
  command -v "$cmd" >/dev/null 2>&1 || {
    echo "Missing required command: $cmd" >&2
    exit 1
  }
}

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
    --log-root)
      LOG_ROOT="$2"
      shift 2
      ;;
    --remote-reconnect)
      REMOTE_RECONNECT_TIMEOUT="$2"
      shift 2
      ;;
    --no-bt-prime)
      BT_PRIME=0
      shift
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
  LOG_ROOT="${BUILD_DIR}/distributed-link-logs/$(date +%Y%m%d-%H%M%S)-bt"
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
  scp -o BatchMode=yes -o ConnectTimeout=8 "${ARTIFACT_PATHS[@]}" "$REMOTE_HOST:$REMOTE_DIR/"
fi

bt_prepare_body() {
  cat <<'EOF'
if command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1; then
  SUDO="sudo -n"
else
  SUDO=""
fi
run_bt_cmd() {
  timeout 3s bash -lc "$1" >/dev/null 2>&1 || true
}
run_bt_cmd "$SUDO rfkill unblock bluetooth"
run_bt_cmd "$SUDO btmgmt power on"
run_bt_cmd "$SUDO btmgmt connectable on"
run_bt_cmd "$SUDO btmgmt bondable on"
run_bt_cmd "$SUDO btmgmt pairable on"
run_bt_cmd "$SUDO btmgmt ssp on"
if command -v bluetoothctl >/dev/null 2>&1; then
  timeout 5s bash -lc "printf 'power on\ndiscoverable on\npairable on\nagent NoInputNoOutput\ndefault-agent\nquit\n' | $SUDO bluetoothctl" >/dev/null 2>&1 || true
fi
EOF
}

prepare_local_bluetooth() {
  bash -lc "$(bt_prepare_body)"
}

prepare_remote_bluetooth() {
  ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" "bash -lc $(printf '%q' "$(bt_prepare_body)")"
}

get_local_bluetooth_mac() {
  bluetoothctl show 2>/dev/null | sed -n 's/^Controller \([^ ]*\).*/\1/p' | head -n 1
}

get_remote_bluetooth_mac() {
  ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" \
    "bluetoothctl show 2>/dev/null | sed -n 's/^Controller \\([^ ]*\\).*/\\1/p' | head -n 1"
}

prime_local_bluetooth_discovery() {
  timeout 15s hcitool inq >/dev/null 2>&1 || true
}

prime_remote_bluetooth_discovery() {
  ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" "timeout 15s hcitool inq >/dev/null 2>&1 || true"
}

prime_remote_bluetooth_cache() {
  local local_mac="$1"
  [[ -n "$local_mac" ]] || return 0
  ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" \
    "sudo -n l2ping -c 2 $(printf '%q' "$local_mac") >/dev/null 2>&1 || true"
}

warm_local_bluetooth_scanner() {
  (
    cd "$BUILD_DIR"
    export LD_LIBRARY_PATH="$BUILD_DIR:${LD_LIBRARY_PATH:-}"
    timeout 20s stdbuf -oL -eL ./instrument-cli bluetooth >/dev/null 2>&1 || true
  )
}

warm_remote_bluetooth_scanner() {
  ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" \
    "cd $(printf '%q' "$REMOTE_DIR") && export LD_LIBRARY_PATH=$(printf '%q' "$REMOTE_DIR"):\${LD_LIBRARY_PATH:-} && timeout 20s stdbuf -oL -eL ./instrument-cli bluetooth >/dev/null 2>&1 || true"
}

run_local_harness() {
  local logfile="$1"

  (
    cd "$BUILD_DIR"
    export LD_LIBRARY_PATH="$BUILD_DIR:${LD_LIBRARY_PATH:-}"
    export SENSOR_LIB_LINK_CALLBACK_SKIP_WIFI_MODE=1
    export SENSOR_LIB_LINK_CALLBACK_SKIP_WIFI_SCAN=1
    export SENSOR_LIB_COMMS_SKIP_WIFI=1
    export SENSOR_LIB_COMMS_PREFER_BLUETOOTH=1
    exec stdbuf -oL -eL timeout -s INT "${TEST_DURATION}s" \
      ./instrument-cli link-callback
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
  local logfile="$1"
  local remote_base remote_log remote_status remote_cmd start_cmd

  remote_base="$(remote_meta_base)"
  remote_log="${remote_base}/remote.log"
  remote_status="${remote_base}/remote.status"

  remote_cmd=$(
    cat <<EOF
cd $(printf '%q' "$REMOTE_DIR")
export LD_LIBRARY_PATH=$(printf '%q' "$REMOTE_DIR"):\${LD_LIBRARY_PATH:-}
export SENSOR_LIB_LINK_CALLBACK_SKIP_WIFI_MODE=1
export SENSOR_LIB_LINK_CALLBACK_SKIP_WIFI_SCAN=1
export SENSOR_LIB_COMMS_SKIP_WIFI=1
export SENSOR_LIB_COMMS_PREFER_BLUETOOTH=1
stdbuf -oL -eL timeout -s INT ${TEST_DURATION}s ./instrument-cli link-callback
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
  local logfile="$1"
  local remote_base remote_log
  remote_base="$(remote_meta_base)"
  remote_log="${remote_base}/remote.log"
  ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" "cat $(printf '%q' "$remote_log")" >"$logfile" 2>&1 || true
}

read_remote_status() {
  local remote_base remote_status
  remote_base="$(remote_meta_base)"
  remote_status="${remote_base}/remote.status"
  ssh -o BatchMode=yes -o ConnectTimeout=8 "$REMOTE_HOST" "cat $(printf '%q' "$remote_status")" 2>/dev/null | tail -n 1
}

wait_for_remote_status() {
  local deadline=$((SECONDS + REMOTE_RECONNECT_TIMEOUT))
  local status=""
  while ((SECONDS < deadline)); do
    if wait_for_remote_ssh; then
      status="$(read_remote_status)"
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

LOCAL_LOG="${LOG_ROOT}/local.log"
REMOTE_LOG="${LOG_ROOT}/remote.log"

echo
echo "== Preparing Bluetooth on both hosts =="
prepare_local_bluetooth
prepare_remote_bluetooth

if ((BT_PRIME != 0)); then
  echo
  echo "== Priming Bluetooth discovery =="
  LOCAL_BT_MAC="$(get_local_bluetooth_mac)"
  REMOTE_BT_MAC="$(get_remote_bluetooth_mac)"
  echo "Local Bluetooth MAC:  ${LOCAL_BT_MAC:-<unknown>}"
  echo "Remote Bluetooth MAC: ${REMOTE_BT_MAC:-<unknown>}"
  prime_local_bluetooth_discovery
  prime_remote_bluetooth_discovery
  prime_remote_bluetooth_cache "$LOCAL_BT_MAC"
  warm_local_bluetooth_scanner
  warm_remote_bluetooth_scanner
fi

echo
echo "== Running BT-only distributed link-callback test =="
run_local_harness "$LOCAL_LOG"
local_pid="$LAST_PID"
run_remote_harness_detached "$REMOTE_LOG"

wait_and_capture_rc "$local_pid"
local_rc="$LAST_WAIT_RC"
if remote_rc="$(wait_for_remote_status)"; then
  collect_remote_log "$REMOTE_LOG"
else
  echo "<remote log/status did not recover within ${REMOTE_RECONNECT_TIMEOUT}s>" >>"$REMOTE_LOG"
  remote_rc="ssh-reconnect-timeout"
fi

cleanup_pids=()

echo "Exit codes: local=$local_rc remote=$remote_rc"
show_log "local output" "$LOCAL_LOG"
show_log "remote output" "$REMOTE_LOG"

echo
echo "Logs saved under: $LOG_ROOT"
