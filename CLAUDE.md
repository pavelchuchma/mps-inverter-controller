# Project Notes

## Credentials / secrets

`include/credentials.h` holds real secrets and is git-ignored — never commit it.
After a fresh clone, create it from the tracked template and enable the safety hook:

```
cp include/credentials.h.example include/credentials.h   # then fill in real values
git config core.hooksPath .githooks                       # blocks committing credentials.h
```

The `.githooks/pre-commit` hook refuses any commit that stages `include/credentials.h`
(even via `git add -f`).

## Uploading web files to ESP32 LittleFS

`pio remote run -t uploadfs` fails (partition table not found on remote agent).
ArduinoOTA doesn't work over WireGuard tunnel (UDP port 3232 not reachable).

**Working method:** HTTP upload endpoint at `/upload` (implemented in `esp_webserver.cpp`).

Upload all three web files via curl:
```
curl -F "file=@data/index.html;filename=index.html" \
     -F "file=@data/app.js;filename=app.js" \
     -F "file=@data/style.css;filename=style.css" \
     http://10.200.0.30/upload
```

**Firmware** is uploaded via `platformio remote run -t upload` (serial, works fine).

Only re-upload web files when `data/` contents change. Firmware-only changes (C++ code) don't require FS re-upload.
