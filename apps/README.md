# Bird Remote implementation

## Architecture

The MacBook runs the dependency-free Node 22 server in `server/`, reads the
curated catalog from `assets/birds/`, and displays the page it serves. The
ESP32 firmware in `main/` sends commands to that server. The server is the sole
owner of current-bird state.

## HTTP interface

- `GET /health` — deterministic `{"status":"ok"}`
- `GET /` — fullscreen display
- `GET /api/current` — current metadata and version
- `GET /api/events` — SSE display updates
- `POST /api/next` — select a different bird; requires a bearer token
- `GET /birds/<file>` — serve a JPEG present in the curated catalog

`POST /api/next` does not immediately repeat a bird. State is persisted across
server restarts. The browser receives changes over SSE and reconnects
automatically.

Run server tests:

```bash
cd server
npm test
```

## Catalog rules

The JPEG directory is authoritative. At startup the server joins existing JPEG
filenames to `metadata.json` for title, artist, license, and source URL. Stale
metadata is logged and ignored. An existing JPEG without complete attribution
fails startup. Generated catalog URLs can never serve deleted images.

`tools/download_bird_images.py` can collect additional freely reusable images
from Wikimedia Commons. Review and curate every download before committing it.
Photographs retain the individual licenses listed in `metadata.json`.

## Firmware

Local, ignored `sdkconfig.defaults.local` contains:

```text
CONFIG_BIRD_REMOTE_SERVER_URL="http://<MACBOOK-LAN-IP>:8080"
CONFIG_BIRD_REMOTE_TOKEN="<shared-token>"
```

`../tools/install-bird-server-macos` generates this file. The token is compiled
into the command client and must match the Mac server's private environment.
The request has a four-second timeout and only one request may be in flight.
There is no automatic mutation retry, avoiding duplicate accepted commands
when a response is lost.

Wi-Fi credentials remain in ESP-IDF NVS. While connected, only the nearly
edge-to-edge pixel-art button is shown. It is disabled during reconnects and
requests, with pressed/success/error feedback on the same control. Provisioning
is available only without credentials or after five failed reconnects.

The conservative hardware settings remain:

```text
CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT=20
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
```

## Build

```bash
source ~/.espressif/tools/activate_idf_v6.0.2.sh
idf.py set-target esp32s3
idf.py build
```

On the MacBook, use `~/bin/bird-flash` after running the installer documented in
the repository root.

## Hardware acceptance

1. Confirm a phone on the ESP32 Wi-Fi receives 200 from the MacBook health URL.
2. Open the display on the MacBook and mirror it to the TV.
3. Leave the ESP32 connected for ten minutes and press it 50 times.
4. Confirm each successful press advances the server version exactly once.
5. Confirm no reset, stack overflow, DMA allocation failure, or Guru Meditation.
