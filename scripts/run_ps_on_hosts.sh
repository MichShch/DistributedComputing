#!/usr/bin/env bash
# run_ps_on_hosts.sh
# Usage:
#   ./run_ps_on_hosts.sh hosts.txt commands.ps1 username [ssh_port]
#
# hosts.txt    — IP/host по одному в строке (можно # комментарии)
# commands.ps1 — PowerShell-скрипт, который будет выполнен на каждом Windows-хосте

set -euo pipefail

HOSTS_FILE="${1:-}"
PS_FILE="${2:-}"
USER_NAME="${3:-}"
SSH_PORT="${4:-22}"

if [[ -z "$HOSTS_FILE" || -z "$PS_FILE" || -z "$USER_NAME" ]]; then
  echo "Usage: $0 hosts.txt commands.ps1 username [ssh_port]" >&2
  exit 1
fi
[[ -f "$HOSTS_FILE" ]] || { echo "ERROR: hosts file not found: $HOSTS_FILE" >&2; exit 1; }
[[ -f "$PS_FILE" ]]    || { echo "ERROR: ps file not found: $PS_FILE" >&2; exit 1; }

LOG_DIR="./ssh_ps_logs"
mkdir -p "$LOG_DIR"

SSH_OPTS=(
  -p "$SSH_PORT"
  -o BatchMode=yes
  -o StrictHostKeyChecking=accept-new
  -o ConnectTimeout=8
  -o ServerAliveInterval=15
  -o ServerAliveCountMax=2
  -o LogLevel=ERROR
)

run_one() {
  local host="$1"
  local ts log_out log_err
  ts="$(date +%Y%m%d_%H%M%S)"
  log_out="$LOG_DIR/${host}_${ts}.out.log"
  log_err="$LOG_DIR/${host}_${ts}.err.log"

  echo "===== [$host] START ====="

  # IMPORTANT:
  # -Command -  => PowerShell читает команды из stdin
  # -ExecutionPolicy Bypass => чтобы не упереться в policy
  # STDOUT/ERR логируем в файлы
  if ssh "${SSH_OPTS[@]}" "${USER_NAME}@${host}" \
      "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command -" \
      < "$PS_FILE" >"$log_out" 2>"$log_err"; then
    echo "===== [$host] OK ===== (logs: $log_out, $log_err)"
    return 0
  else
    local rc=$?
    echo "===== [$host] FAIL rc=$rc ===== (logs: $log_out, $log_err)" >&2
    return $rc
  fi
}

overall_rc=0
while IFS= read -r line || [[ -n "$line" ]]; do
  host="$(echo "$line" | sed 's/[[:space:]]//g')"
  [[ -z "$host" ]] && continue
  [[ "$host" =~ ^# ]] && continue

  if ! run_one "$host"; then
    overall_rc=1
  fi
done < "$HOSTS_FILE"

exit "$overall_rc"