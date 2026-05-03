#!/usr/bin/env bash
set -euo pipefail

PHONE_HOST="${PHONE_HOST:-192.168.68.2}"
PHONE_USER="${PHONE_USER:-u0_a173}"
PHONE_PORT="${PHONE_PORT:-8022}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "==> Copying battery_server.py to $PHONE_USER@$PHONE_HOST:~"
scp -P "$PHONE_PORT" \
    "$SCRIPT_DIR/src/battery_server.py" \
    "$PHONE_USER@$PHONE_HOST:battery_server.py"

echo "==> Restarting server on the phone"
ssh -p "$PHONE_PORT" "$PHONE_USER@$PHONE_HOST" 'bash -s' <<'REMOTE'
set -e
chmod +x ~/battery_server.py

if pgrep -f battery_server.py >/dev/null; then
  echo "  stopping old server: $(pgrep -fa battery_server.py | head -1)"
  pkill -f battery_server.py
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    pgrep -f battery_server.py >/dev/null || break
    sleep 0.2
  done
fi

echo "  starting new server (log: ~/battery_server.log)"
nohup ~/.termux/boot/start-battery-server </dev/null >~/battery_server.log 2>&1 &

echo "  smoke test (cold termux-api can take ~10 s)"
deadline=$(( $(date +%s) + 30 ))
code=000
elapsed=0
while [ "$(date +%s)" -lt "$deadline" ]; do
  out=$(curl -sS --max-time 20 -o /dev/null \
           -w '%{http_code} %{time_total}' \
           http://127.0.0.1:8080/battery 2>/dev/null || true)
  code="${out%% *}"
  elapsed="${out##* }"
  [ "$code" = "200" ] && break
  sleep 0.5
done

if [ "$code" != "200" ]; then
  echo "  FAILED: last HTTP=$code (after ${elapsed}s)"
  echo "  python process: $(pgrep -fa battery_server.py | head -1 || echo NONE)"
  echo "  ~/battery_server.log:"
  tail -20 ~/battery_server.log || true
  exit 1
fi
echo "  HTTP 200 in ${elapsed}s — pid $(pgrep -f battery_server.py | head -1)"
echo "OK"
REMOTE
