#!/usr/bin/env bash
set -euo pipefail

PHONE_HOST="${PHONE_HOST:-192.168.68.10}"
PHONE_USER="${PHONE_USER:-u0_a173}"
PHONE_PORT="${PHONE_PORT:-8022}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> Syncing boot scripts to $PHONE_USER@$PHONE_HOST:~/.termux/boot/"
scp -P "$PHONE_PORT" \
    "$SCRIPT_DIR"/boot/start-* \
    "$PHONE_USER@$PHONE_HOST:.termux/boot/"

echo "==> Copying status_server.py to $PHONE_USER@$PHONE_HOST:~"
scp -P "$PHONE_PORT" \
    "$SCRIPT_DIR/src/status_server.py" \
    "$PHONE_USER@$PHONE_HOST:status_server.py"

echo "==> Restarting server on the phone"
ssh -p "$PHONE_PORT" "$PHONE_USER@$PHONE_HOST" 'bash -s' <<'REMOTE'
set -e
chmod +x ~/.termux/boot/start-* ~/status_server.py

if pgrep -f status_server.py >/dev/null; then
  echo "  stopping old server: $(pgrep -fa status_server.py | head -1)"
  pkill -f status_server.py
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    pgrep -f status_server.py >/dev/null || break
    sleep 0.2
  done
fi

echo "  starting new server (log: ~/status_server.log)"
nohup ~/.termux/boot/start-status-server </dev/null >~/status_server.log 2>&1 &

echo "  smoke test (cold termux-api can take ~10 s)"
deadline=$(( $(date +%s) + 30 ))
code=000
elapsed=0
while [ "$(date +%s)" -lt "$deadline" ]; do
  out=$(curl -sS --max-time 20 -o /dev/null \
           -w '%{http_code} %{time_total}' \
           http://127.0.0.1:8080/status 2>/dev/null || true)
  code="${out%% *}"
  elapsed="${out##* }"
  [ "$code" = "200" ] && break
  sleep 0.5
done

if [ "$code" != "200" ]; then
  echo "  FAILED: last HTTP=$code (after ${elapsed}s)"
  echo "  python process: $(pgrep -fa status_server.py | head -1 || echo NONE)"
  echo "  ~/status_server.log:"
  tail -20 ~/status_server.log || true
  exit 1
fi
echo "  HTTP 200 in ${elapsed}s — pid $(pgrep -f status_server.py | head -1)"
echo "OK"
REMOTE
