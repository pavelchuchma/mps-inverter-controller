# Phone Status Server (Termux) – Setup How-To

## Goal

The ESP32 connects to the internet through a phone WiFi hotspot. The ESP needs
the phone's battery state (level, charging status, temperature) and per-interface
network byte counters (for the 15 GB/year cellular cap). Both are exposed from
the phone over LAN as a single HTTP/JSON endpoint `/status` and the ESP polls it.

```
ESP32  ──HTTP GET /status──▶  http://<phone-gateway-ip>:8080
                                 │
                                 └─ Termux Python server
                                       ├─ termux-battery-status (CLI) ──▶ Termux:API ──▶ Android battery API
                                       └─ /proc/net/dev (per-iface byte counters)
```

From the ESP's point of view the phone IP is stable: it is the hotspot gateway
(`WiFi.gatewayIP()`), regardless of the (DHCP-assigned, possibly changing) ESP IP.

## Components on the phone

All three apps must come from **F-Droid** (or all three from Play Store). The
Termux ecosystem apps must share the same signing key, otherwise Termux:API and
Termux:Boot cannot talk to Termux. F-Droid builds are the safe default.

| App         | Purpose                                                    |
|-------------|------------------------------------------------------------|
| Termux      | Linux userland + shell                                     |
| Termux:API  | Bridges `termux-*` CLI tools to Android APIs               |
| Termux:Boot | Runs `~/.termux/boot/*` scripts after device boot + unlock |

After installing Termux:Boot, **open it once**. It has no UI (just a blank
screen) but the first launch is what registers the Android boot receiver.

## One-time setup inside Termux

```bash
pkg update
pkg install python termux-api openssh   # openssh optional, see "Related"

# verify Termux:API works (must return JSON):
termux-battery-status
```

If `termux-battery-status` hangs or errors, the Termux:API app is missing or its
permissions were not granted — fix that first.

## Status server script

The canonical source lives in this repo at
[phone/src/status_server.py](../phone/src/status_server.py) and is deployed
to the phone as `~/status_server.py` (i.e.
`/data/data/com.termux/files/home/status_server.py`).

Design constraints that are baked into the script — change them only with care:

- **Two background poller threads + in-memory cache.** The HTTP handler never
  blocks on I/O; it just reads the in-memory snapshots, so each `/status`
  request is sub-millisecond.
  - **Battery** thread calls `termux-battery-status` every
    `BATTERY_POLL_INTERVAL=10 s` and stores the parsed JSON (the response
    additionally exposes `stale_secs` so the client can tell when Termux:API
    has gone cold or wedged).
  - **Network** thread reads `/proc/net/dev` every `NETWORK_POLL_INTERVAL=60 s`
    and accumulates per-interface RX/TX bytes into totals. Kernel counters are
    cumulative-since-iface-up and reset on interface down/up or reboot; the
    poller detects a reset as `current < last_raw` and treats the *current*
    value as a fresh baseline (the small delta between the last poll and the
    reset is intentionally dropped — bounded by `NETWORK_POLL_INTERVAL`).
    Tracked interfaces are configured via `NETWORK_TRACKED_INTERFACES` at the
    top of the script; missing interfaces are silently skipped. The cellular
    ifname (e.g. `rmnet0`, `rmnet_data0`) varies by device — discover it via
    `ip route | awk '/default/ {print $5}'` once cellular data is up.
    The response exposes `stale_secs` (seconds since the last successful
    poll) so clients can detect a wedged poller without parsing timestamps;
    a value > ~120 s means the poller has stopped. The on-disk file
    additionally stores the absolute `updated` (ISO) for human-readable
    debugging.
- **`timeout=15`** on the battery poller's `subprocess.run(['termux-battery-status'])`.
  Must be ≥ the worst-case cold start (~10 s after Termux:API has been idle and
  Android killed its helper). On `TimeoutExpired` the script immediately runs
  `pkill -9 -f 'termux-api BatteryStatus'` to reap the Android-side helper that
  `subprocess.run` could not. Without this reaping, each timeout leaks one
  helper blocked on a FIFO, and just a handful of leaked helpers is enough to
  wedge the Termux:API service so that *every* subsequent call hangs.
- **`~/network_stats.json` persistence.** Network totals are saved to disk
  every `NETWORK_SAVE_INTERVAL=300 s` via write-to-`.tmp` + `os.replace`
  (atomic rename — never leaves a half-written file even on crash). On
  startup the file is loaded back; `last_raw_*` is *not* restored — the first
  poll after restart always reseeds the baseline because kernel counters
  reset across reboot.
- **`stale_secs` and `last_error` in the battery response.** Clients should
  treat a large `stale_secs` (e.g. > 60 s) as "phone is unreachable /
  Termux:API wedged" and degrade accordingly rather than blocking on a fresh
  fetch.
- **`HTTPServer(...).serve_forever()`** on the last line, and the poller threads
  started as `daemon=True` just above it. Without `serve_forever`, the script
  defines the class and exits immediately (the `nohup` wrapper then has
  nothing to keep alive — symptom: `pgrep` finds no python after start).
- **`0.0.0.0`** so the ESP on the hotspot LAN can reach it (not just localhost).
- **`log_message` overridden to no-op** — silences the per-request stderr line
  that would otherwise spam `~/status_server.log` and Termux notifications.

## Autostart at boot

Termux:Boot runs every executable file in `~/.termux/boot/` after the device
boots **and** the user unlocks for the first time (boot scripts cannot run
before unlock — the Termux storage is part of the encrypted user partition).

Create `~/.termux/boot/start-status-server`:

```bash
#!/data/data/com.termux/files/usr/bin/sh
termux-wake-lock
exec python3 /data/data/com.termux/files/home/status_server.py
```

```bash
mkdir -p ~/.termux/boot
chmod +x ~/.termux/boot/start-status-server
```

Why these specific lines:

- Use the **absolute path** to the script. `~` is not always expanded by the
  Termux:Boot launcher.
- `termux-wake-lock` keeps Android from suspending the Termux process (and the
  Python HTTP server with it) once the screen is off.
- `exec` replaces the shell with python so there is one less process to track.

## Disable battery optimization

Android will otherwise put Termux to sleep within minutes. For **all three**
apps:

`Settings → Apps → Termux → Battery → Unrestricted`
`Settings → Apps → Termux:Boot → Battery → Unrestricted`
`Settings → Apps → Termux:API → Battery → Unrestricted`

Termux:API is the easiest to forget but the most damaging to miss: when
Android suspends the `com.termux.api` service while it has pending
`termux-battery-status` requests, the helpers are left blocked on a FIFO and
the service wedges for every subsequent caller until force-stopped (see
[phone/src/status_server.py](../phone/src/status_server.py) for the
in-process pkill mitigation, but prevention is better).

### Samsung-specific (Samsung Galaxy A3, the phone used in this project)

On Samsung devices the per-app `Battery → Unrestricted` toggle above is **not
sufficient** — Samsung has a separate "monitored / sleeping apps" list that
overrides it and forcibly suspends apps shortly after the screen turns off.
Symptom: `termux-battery-status` hangs as soon as the screen has been off for
a few minutes, even though Termux itself (and `sshd`) keep running.

Fix — add all three Termux apps to the *Unmonitored Apps* list:

`Settings → Device Maintenance → Battery → Unmonitored Apps → +`
→ select **Termux**, **Termux:Boot**, **Termux:API**.

(On newer Samsung UIs the same setting may be under *Device care → Battery →
Background usage limits → Never sleeping apps*.) Without this, the standard
`Apps → … → Battery → Unrestricted` change is silently ignored once the
screen turns off.

### Trigger the system dialog from Termux

On newer Androids you can also trigger the system dialog from Termux:

```bash
am start --user 0 -a android.settings.REQUEST_IGNORE_BATTERY_OPTIMIZATIONS \
         -d package:com.termux
```

## Updating the server (`phone/deploy.sh`)

Once the phone is bootstrapped (apps installed, boot scripts in place, sshd
running), code changes in `phone/src/status_server.py` are pushed with:

```bash
cd phone
./deploy.sh
```

The script is at [phone/deploy.sh](../phone/deploy.sh). It:

1. `scp`s `src/status_server.py` to `~/status_server.py` over port 8022.
2. `chmod +x` on the deployed file.
3. `pkill -f status_server.py`, waits up to 2 s for the process to exit
   (avoids a port race where the new python tries to bind before the old one
   has released `0.0.0.0:8080`).
4. Restarts via `nohup ~/.termux/boot/start-status-server …` — deliberately
   the *same* boot wrapper used at device startup, so wake-lock semantics
   match. stdout/stderr go to `~/status_server.log` (handy for post-mortem if
   the new server fails to start).
5. Polls `curl http://127.0.0.1:8080/status` for up to 30 s. Expects HTTP 200.
   Prints the response time so you can see whether termux-api was warm
   (~50 ms) or cold (~5–10 s).

Override the target with env vars:

```bash
PHONE_HOST=10.200.0.5 PHONE_USER=u0_a173 PHONE_PORT=8022 ./deploy.sh
```

The script exits non-zero with `~/status_server.log` tail on any failure
(scp, kill, restart, smoke test).

> **Self-match gotcha** when debugging on the phone: `pkill -f <pattern>`
> matches against `/proc/*/cmdline`, including the bash that invoked it. If
> your interactive shell command contains the pattern, pkill will kill its own
> parent. Use `bash -s` heredoc style (the script comes via stdin so the
> bash cmdline is just `bash -s`) or pre-resolve PIDs with `pgrep` and
> `kill <pid>`.

## Verify after reboot

1. Reboot the phone.
2. Unlock once (boot scripts wait for the first unlock).
3. Wait ~10 s.
4. From the laptop on the same hotspot:

   ```bash
   curl http://<phone-ip>:8080/status
   # {
   #   "battery": {"health":"GOOD","percentage":85,"plugged":"UNPLUGGED",
   #               "temperature":29.4,"stale_secs":3.2, ...},
   #   "network": {"since":"2026-05-17T13:50:00+00:00",
   #               "stale_secs":12.3,
   #               "interfaces":{"wlan0":{"rx_bytes":388054,"tx_bytes":301472}}}
   # }
   ```

5. From the ESP side, the URL is built from `WiFi.gatewayIP()`:

   ```cpp
   String url = "http://" + WiFi.gatewayIP().toString() + ":8080/status";
   ```

If something does not respond:

- `~/.termux/boot.log` — Termux:Boot writes script stdout/stderr here.
- `pgrep -fa status_server` — is the python server actually running?
- `curl http://127.0.0.1:8080/status` from inside Termux — isolates network
  vs. server problems.

## Related on-phone setup (separate scripts in `~/.termux/boot/`)

Termux:Boot runs **all** files in the boot directory; add one file per service.

- **sshd** — for remote shell into the phone over the WG tunnel:

  ```bash
  # ~/.termux/boot/start-sshd
  #!/data/data/com.termux/files/usr/bin/sh
  sshd
  ```

  Termux `sshd` listens on **port 8022** (no root → cannot bind privileged
  ports). Username is whatever `whoami` reports (e.g. `u0_a173`). Prefer key
  auth via `~/.ssh/authorized_keys` (`chmod 600`).

- **WireGuard** — **does not work on the phone in this project** (older
  Android, kernel `3.18.14` armv8l). The official WireGuard Android app was
  attempted (it uses userspace `wireguard-go` and does not need root) but the
  tunnel did not come up reliably. Left disabled; the phone is reached over
  the hotspot LAN only. Recorded here so this path is not retried on this
  device. Note also that WireGuard inside Termux is not an option on a
  non-rooted phone — no TUN access.
