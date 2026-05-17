#!/usr/bin/env python3
import json
import os
import subprocess
import threading
import time
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = 8080

BATTERY_POLL_INTERVAL = 10
BATTERY_SUBPROCESS_TIMEOUT = 15
BATTERY_POLL_ERROR_BACKOFF = 5

NETWORK_TRACKED_INTERFACES = ('wlan0', 'rmnet0')
NETWORK_POLL_INTERVAL = 60
NETWORK_SAVE_INTERVAL = 300
NETWORK_STATE_FILE = os.path.expanduser('~/network_stats.json')


_battery_lock = threading.Lock()
_battery_last = None
_battery_last_ok_ts = 0.0
_battery_last_err = None

_network_lock = threading.Lock()
_network_state = {}
_network_since = None
_network_last_ok_iso = None


def _kill_leaked_helpers():
    # Each subprocess timeout leaves /usr/libexec/termux-api BatteryStatus
    # blocked on a FIFO. A few of these wedge the Termux:API service for every
    # subsequent caller, so reap aggressively after each failure.
    subprocess.run(
        ['pkill', '-9', '-f', 'termux-api BatteryStatus'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )


def _battery_poll_once():
    global _battery_last, _battery_last_ok_ts, _battery_last_err
    try:
        out = subprocess.run(
            ['termux-battery-status'],
            capture_output=True, text=True,
            timeout=BATTERY_SUBPROCESS_TIMEOUT, check=True,
        ).stdout
        data = json.loads(out)
        with _battery_lock:
            _battery_last = data
            _battery_last_ok_ts = time.monotonic()
            _battery_last_err = None
        return True
    except subprocess.TimeoutExpired as e:
        _kill_leaked_helpers()
        with _battery_lock:
            _battery_last_err = f'timeout: {e}'
        return False
    except Exception as e:
        with _battery_lock:
            _battery_last_err = f'{type(e).__name__}: {e}'
        return False


def _battery_poller():
    while True:
        ok = _battery_poll_once()
        time.sleep(BATTERY_POLL_INTERVAL if ok else BATTERY_POLL_ERROR_BACKOFF)


def _now_iso():
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def _read_proc_net_dev():
    out = {}
    with open('/proc/net/dev') as f:
        for line in f:
            if ':' not in line:
                continue
            name, rest = line.split(':', 1)
            name = name.strip()
            fields = rest.split()
            if len(fields) < 10:
                continue
            out[name] = (int(fields[0]), int(fields[8]))
    return out


def _network_load():
    global _network_since
    try:
        with open(NETWORK_STATE_FILE) as f:
            data = json.load(f)
    except FileNotFoundError:
        _network_since = _now_iso()
        return
    except Exception:
        try:
            os.rename(NETWORK_STATE_FILE, NETWORK_STATE_FILE + '.broken')
        except OSError:
            pass
        _network_since = _now_iso()
        return
    _network_since = data.get('since') or _now_iso()
    for name, st in data.get('interfaces', {}).items():
        # last_raw_* discarded — kernel counters reset across reboot, so the
        # first poll after restart must just reseed the baseline.
        _network_state[name] = {
            'total_rx': int(st.get('total_rx', 0)),
            'total_tx': int(st.get('total_tx', 0)),
            'last_raw_rx': None,
            'last_raw_tx': None,
        }


def _network_save_locked():
    data = {
        'since': _network_since,
        'updated': _network_last_ok_iso or _now_iso(),
        'interfaces': {
            name: {
                'total_rx': st['total_rx'],
                'total_tx': st['total_tx'],
            }
            for name, st in _network_state.items()
        },
    }
    tmp = NETWORK_STATE_FILE + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(data, f, indent=2)
    os.replace(tmp, NETWORK_STATE_FILE)


def _network_poll_once():
    global _network_last_ok_iso
    try:
        raw = _read_proc_net_dev()
    except OSError:
        return
    with _network_lock:
        for name in NETWORK_TRACKED_INTERFACES:
            cur = raw.get(name)
            if cur is None:
                continue
            cur_rx, cur_tx = cur
            st = _network_state.setdefault(name, {
                'total_rx': 0, 'total_tx': 0,
                'last_raw_rx': None, 'last_raw_tx': None,
            })
            if st['last_raw_rx'] is not None:
                drx = cur_rx - st['last_raw_rx']
                if drx < 0:
                    # Counter reset (iface down/up or reboot); the small delta
                    # between the last poll and the reset is lost on purpose.
                    drx = cur_rx
                dtx = cur_tx - st['last_raw_tx']
                if dtx < 0:
                    dtx = cur_tx
                st['total_rx'] += drx
                st['total_tx'] += dtx
            st['last_raw_rx'] = cur_rx
            st['last_raw_tx'] = cur_tx
        _network_last_ok_iso = _now_iso()


def _network_poller():
    _network_load()
    last_save = time.monotonic()
    while True:
        _network_poll_once()
        if time.monotonic() - last_save >= NETWORK_SAVE_INTERVAL:
            try:
                with _network_lock:
                    _network_save_locked()
                last_save = time.monotonic()
            except Exception:
                pass
        time.sleep(NETWORK_POLL_INTERVAL)


def _battery_snapshot():
    with _battery_lock:
        data = _battery_last
        ok_ts = _battery_last_ok_ts
        err = _battery_last_err
    if data is None:
        return {'error': err or 'no battery data yet'}
    body = dict(data)
    body['stale_secs'] = round(time.monotonic() - ok_ts, 1)
    if err is not None:
        body['last_error'] = err
    return body


def _network_snapshot():
    with _network_lock:
        body = {
            'since': _network_since,
            'interfaces': {
                name: {
                    'rx_bytes': st['total_rx'],
                    'tx_bytes': st['total_tx'],
                }
                for name, st in _network_state.items()
            },
        }
        if _network_last_ok_iso is not None:
            elapsed = (datetime.now(timezone.utc)
                       - datetime.fromisoformat(_network_last_ok_iso)).total_seconds()
            body['stale_secs'] = round(elapsed, 1)
    return body


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != '/status':
            self.send_error(404)
            return
        self._write_json({
            'battery': _battery_snapshot(),
            'network': _network_snapshot(),
        })

    def _write_json(self, body):
        payload = json.dumps(body).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *a):
        pass


threading.Thread(target=_battery_poller, daemon=True).start()
threading.Thread(target=_network_poller, daemon=True).start()
HTTPServer(('0.0.0.0', PORT), Handler).serve_forever()
