#!/usr/bin/env bash
#
# remote-monitor.sh — workaround for the intermittent wrong-baud problem on the
# remote serial bridge (CP2102 on "pizero", reached via `pio remote`).
#
# By default the script simply opens the remote monitor at the correct baud
# rate. Occasionally the monitor comes up at the wrong effective baud rate and
# prints garbage; manually toggling the client baud rate (wrong -> correct)
# fixes it. Pass `-r` to automate that toggle (a "refresh") before connecting:
#
#   1. Open the remote monitor at a deliberately WRONG baud rate.
#   2. As soon as a few bytes arrive (proof the port opened and the CP210x
#      divisor was reprogrammed), stop that monitor.
#   3. Reopen the monitor at the correct (default) baud rate, foreground,
#      so you get a normal interactive session.
#
# Usage:
#   scripts/remote-monitor.sh        connect directly at GOOD_BAUD
#   scripts/remote-monitor.sh -r     prime at WRONG_BAUD first, then connect
#
# Tunables via environment variables:
#   WRONG_BAUD     baud used for the priming pass         (default 9600)
#   GOOD_BAUD      baud for the real session              (default: project default)
#   PRIME_BYTES    stop priming after this many bytes     (default 8)
#   PRIME_TIMEOUT  max seconds to wait while priming      (default 10)
set -euo pipefail

WRONG_BAUD="${WRONG_BAUD:-9600}"
GOOD_BAUD="${GOOD_BAUD:-115200}"
PRIME_BYTES="${PRIME_BYTES:-8}"
PRIME_TIMEOUT="${PRIME_TIMEOUT:-10}"

refresh=0
while [ $# -gt 0 ]; do
  case "$1" in
    -r|--refresh) refresh=1 ;;
    -h|--help)
      sed -n '2,/^set -euo/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) echo "Unknown option: $1 (try -h)" >&2; exit 2 ;;
  esac
  shift
done

# Run from the project root so `pio` picks up platformio.ini defaults.
cd "$(dirname "$0")/.."

if [ "$refresh" -eq 0 ]; then
  echo ">>> Connecting at baud ${GOOD_BAUD} (use -r to prime the bridge first)..."
  exec pio remote device monitor --baud "${GOOD_BAUD}" --filter time
fi

tmp="$(mktemp)"
mon_pid=""
cleanup() {
  [ -n "$mon_pid" ] && kill "$mon_pid" 2>/dev/null || true
  rm -f "$tmp"
}
trap cleanup EXIT

echo ">>> Priming remote serial bridge at WRONG baud ${WRONG_BAUD}..."

# Launch the priming monitor in the background, capturing its output. stdin is
# detached (/dev/null) so the monitor does not try to grab the keyboard.
pio remote device monitor --baud "${WRONG_BAUD}" >"$tmp" 2>&1 </dev/null &
mon_pid=$!

# pio prints a banner ending with a line that contains "Quit:". Wait for that,
# then count only the device bytes that arrive afterwards.
deadline=$(( SECONDS + PRIME_TIMEOUT ))
banner_seen=0
baseline=0
while kill -0 "$mon_pid" 2>/dev/null; do
  if [ "$banner_seen" -eq 0 ]; then
    if grep -q "Quit:" "$tmp" 2>/dev/null; then
      banner_seen=1
      baseline=$(wc -c <"$tmp" | tr -d ' ')
    fi
  else
    now=$(wc -c <"$tmp" | tr -d ' ')
    if [ "$(( now - baseline ))" -ge "$PRIME_BYTES" ]; then
      echo ">>> Got $(( now - baseline )) bytes — bridge primed."
      break
    fi
  fi
  if [ "$SECONDS" -ge "$deadline" ]; then
    echo ">>> Timeout (${PRIME_TIMEOUT}s) while priming — continuing anyway."
    break
  fi
  sleep 0.2
done

# Stop the priming monitor and wait for it to release the remote port.
kill "$mon_pid" 2>/dev/null || true
wait "$mon_pid" 2>/dev/null || true
mon_pid=""
sleep 1

echo ">>> Reconnecting at correct baud ${GOOD_BAUD}..."
exec pio remote device monitor --baud "${GOOD_BAUD}" --filter time
