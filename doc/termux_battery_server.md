# Phone Battery Server (Termux) – Setup How-To

## Goal

The ESP32 connects to the internet through a phone WiFi hotspot. The ESP needs the
phone's battery state (level, charging status, temperature). We expose it from the
phone over LAN as a tiny HTTP/JSON endpoint and the ESP polls it.

```
ESP32  ──HTTP GET──▶  http://<phone-gateway-ip>:8080/battery
                         │
                         └─ Termux Python server  ──▶  termux-battery-status (CLI)
                                                          │
                                                          └─ Termux:API app  ──▶  Android battery API
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

## Battery server script

The canonical source lives in this repo at
[phone/src/battery_server.py](../phone/src/battery_server.py) and is deployed
to the phone as `~/battery_server.py` (i.e.
`/data/data/com.termux/files/home/battery_server.py`).

Design constraints that are baked into the script — change them only with care:

- **`HTTPServer(...).serve_forever()`** on the last line. Without it the script
  defines the class and exits immediately (the `nohup` wrapper then has nothing
  to keep alive — symptom: `pgrep` finds no python after start).
- **`timeout=15`** on `subprocess.run(['termux-battery-status'])`. This is not
  just a politeness limit — it must be ≥ the worst-case cold start of
  `termux-battery-status` (~10 s after Termux:API has been idle and Android
  killed its helper). When the timeout fires, `subprocess.run` kills the wrapper
  but **leaves the Android-side `/usr/libexec/termux-api BatteryStatus` helper
  running**, blocked on the FIFO that nobody reads. Each timeout leaks one
  zombie helper; after a couple dozen, the Termux:API service wedges and
  *every* subsequent call hangs (recovery requires `pkill -f 'termux-api
  BatteryStatus'`). With 15 s, cold start completes and no zombie is created.
- **`0.0.0.0`** so the ESP on the hotspot LAN can reach it (not just localhost).
- **`log_message` overridden to no-op** — silences the per-request stderr line
  that would otherwise spam `~/battery_server.log` and Termux notifications.

## Autostart at boot

Termux:Boot runs every executable file in `~/.termux/boot/` after the device
boots **and** the user unlocks for the first time (boot scripts cannot run
before unlock — the Termux storage is part of the encrypted user partition).

Create `~/.termux/boot/start-battery-server`:

```bash
#!/data/data/com.termux/files/usr/bin/sh
termux-wake-lock
exec python3 /data/data/com.termux/files/home/battery_server.py
```

```bash
mkdir -p ~/.termux/boot
chmod +x ~/.termux/boot/start-battery-server
```

Why these specific lines:

- Use the **absolute path** to the script. `~` is not always expanded by the
  Termux:Boot launcher.
- `termux-wake-lock` keeps Android from suspending the Termux process (and the
  Python HTTP server with it) once the screen is off.
- `exec` replaces the shell with python so there is one less process to track.

## Disable battery optimization

Android will otherwise put Termux to sleep within minutes. For both apps:

`Settings → Apps → Termux → Battery → Unrestricted`
`Settings → Apps → Termux:Boot → Battery → Unrestricted`

On newer Androids you can also trigger the system dialog from Termux:

```bash
am start --user 0 -a android.settings.REQUEST_IGNORE_BATTERY_OPTIMIZATIONS \
         -d package:com.termux
```

## Updating the server (`phone/deploy.sh`)

Once the phone is bootstrapped (apps installed, boot scripts in place, sshd
running), code changes in `phone/src/battery_server.py` are pushed with:

```bash
cd phone
./deploy.sh
```

The script is at [phone/deploy.sh](../phone/deploy.sh). It:

1. `scp`s `src/battery_server.py` to `~/battery_server.py` over port 8022.
2. `chmod +x` on the deployed file.
3. `pkill -f battery_server.py`, waits up to 2 s for the process to exit
   (avoids a port race where the new python tries to bind before the old one
   has released `0.0.0.0:8080`).
4. Restarts via `nohup ~/.termux/boot/start-battery-server …` — deliberately
   the *same* boot wrapper used at device startup, so wake-lock semantics
   match. stdout/stderr go to `~/battery_server.log` (handy for post-mortem if
   the new server fails to start).
5. Polls `curl http://127.0.0.1:8080/battery` for up to 30 s. Expects HTTP 200.
   Prints the response time so you can see whether termux-api was warm
   (~50 ms) or cold (~5–10 s).

Override the target with env vars:

```bash
PHONE_HOST=10.200.0.5 PHONE_USER=u0_a173 PHONE_PORT=8022 ./deploy.sh
```

The script exits non-zero with `~/battery_server.log` tail on any failure
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
   curl http://<phone-ip>:8080/battery
   # {"health":"GOOD","percentage":85,"plugged":"UNPLUGGED","temperature":29.4, ...}
   ```

5. From the ESP side, the URL is built from `WiFi.gatewayIP()`:

   ```cpp
   String url = "http://" + WiFi.gatewayIP().toString() + ":8080/battery";
   ```

If something does not respond:

- `~/.termux/boot.log` — Termux:Boot writes script stdout/stderr here.
- `pgrep -fa battery_server` — is the python server actually running?
- `curl http://127.0.0.1:8080/battery` from inside Termux — isolates network
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

- **vnstat** for cellular data accounting (the phone has a 15 GB/year cap):

  ```bash
  pkg install vnstat
  vnstat -i <cellular-iface> --add   # iface from `ip route | awk '/default/ {print $5}'`
  # ~/.termux/boot/start-vnstatd
  #!/data/data/com.termux/files/usr/bin/sh
  vnstatd -d
  ```

  vnstat handles counter resets across hotspot toggle / modem restart and
  exposes daily/monthly/yearly stats via `vnstat --json`.
