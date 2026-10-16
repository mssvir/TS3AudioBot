#!/usr/bin/env bash
set -Eeuo pipefail

fixture="${1:-}"
if [[ -z "$fixture" || ! -x "$fixture" ]]; then
  echo "usage: systemd_cgroup_integration.sh <process-tree-fixture>" >&2
  exit 2
fi

fixture="$(realpath "$fixture")"

if [[ "$(ps -p 1 -o comm= | tr -d '[:space:]')" != "systemd" ]]; then
  echo "systemd must be PID 1 for the controlled cgroup integration test" >&2
  exit 3
fi

if [[ "$(stat -fc %T /sys/fs/cgroup)" != "cgroup2fs" ]]; then
  echo "cgroup v2 is required for the controlled integration test" >&2
  exit 4
fi

unit="mssv-native-cgroup-${GITHUB_RUN_ID:-0}-${GITHUB_RUN_ATTEMPT:-0}-$$.service"
marker="${RUNNER_TEMP:-/tmp}/${unit}.child.pid"
control_group=""
child_pid=""

cleanup() {
  sudo systemctl stop "$unit" >/dev/null 2>&1 || true
  sudo systemctl reset-failed "$unit" >/dev/null 2>&1 || true

  if [[ -z "$child_pid" && -r "$marker" ]]; then
    child_pid="$(tr -dc '0-9' < "$marker")"
  fi
  if [[ "$child_pid" =~ ^[1-9][0-9]*$ ]] && kill -0 "$child_pid" 2>/dev/null; then
    sudo kill -KILL "$child_pid" >/dev/null 2>&1 || true
  fi

  rm -f "$marker" >/dev/null 2>&1 || sudo rm -f "$marker" >/dev/null 2>&1 || true
}
trap cleanup EXIT

rm -f "$marker"

sudo systemd-run \
  --unit="$unit" \
  --property=Type=simple \
  --property=KillMode=control-group \
  --property=TimeoutStopSec=750ms \
  --property=SendSIGKILL=yes \
  -- "$fixture" tree-escaped-stubborn "$marker"

for _ in $(seq 1 100); do
  if [[ -s "$marker" ]]; then
    break
  fi
  sleep 0.05
done

if [[ ! -s "$marker" ]]; then
  echo "escaped descendant did not publish its PID" >&2
  sudo systemctl status "$unit" --no-pager || true
  exit 5
fi

child_pid="$(tr -dc '0-9' < "$marker")"
if [[ ! "$child_pid" =~ ^[1-9][0-9]*$ ]]; then
  echo "invalid descendant PID marker" >&2
  exit 6
fi

main_pid="$(sudo systemctl show "$unit" --property=MainPID --value)"
control_group="$(sudo systemctl show "$unit" --property=ControlGroup --value)"

if [[ ! "$main_pid" =~ ^[1-9][0-9]*$ ]]; then
  echo "transient systemd unit has no valid MainPID" >&2
  exit 7
fi
if [[ -z "$control_group" || "$control_group" != /* ]]; then
  echo "transient systemd unit has no valid cgroup path" >&2
  exit 8
fi

main_pgid="$(ps -o pgid= -p "$main_pid" | tr -d '[:space:]')"
child_pgid="$(ps -o pgid= -p "$child_pid" | tr -d '[:space:]')"
child_sid="$(ps -o sid= -p "$child_pid" | tr -d '[:space:]')"

if [[ "$child_pgid" != "$child_pid" || "$child_sid" != "$child_pid" ]]; then
  echo "fixture descendant did not escape into its own session/process group" >&2
  exit 9
fi
if [[ "$child_pgid" == "$main_pgid" ]]; then
  echo "escaped descendant unexpectedly remained in the main process group" >&2
  exit 10
fi

child_cgroup="$(awk -F: '$1 == "0" { print $3 }' "/proc/$child_pid/cgroup")"
if [[ "$child_cgroup" != "$control_group" ]]; then
  echo "escaped descendant left the transient unit cgroup: expected '$control_group', got '$child_cgroup'" >&2
  exit 11
fi

sudo systemctl stop "$unit"

for _ in $(seq 1 100); do
  if ! kill -0 "$child_pid" 2>/dev/null; then
    break
  fi
  sleep 0.05
done

if kill -0 "$child_pid" 2>/dev/null; then
  echo "systemd stop left the escaped SIGTERM-ignoring descendant alive" >&2
  exit 12
fi

cgroup_path="/sys/fs/cgroup${control_group}"
if [[ -e "$cgroup_path/cgroup.procs" && -s "$cgroup_path/cgroup.procs" ]]; then
  echo "systemd stop left processes in the transient unit cgroup" >&2
  cat "$cgroup_path/cgroup.procs" >&2 || true
  exit 13
fi

echo "mssv systemd/cgroup integration test: ok"
