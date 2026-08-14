# Bird Remote

A child-friendly bird-picture remote for a Samsung Frame TV.

```text
ESP32 touchscreen --authenticated command--> MacBook bird server
                                              |             |
                                      curated JPEGs     browser --AirPlay--> TV
```

The MacBook is already required for AirPlay, so it owns the bird catalog,
current state, and display page. The ESP32 remains a small one-button client.
GitHub is the source of truth; runtime secrets stay only on the MacBook.

## MacBook setup

Requirements: macOS, Git, Node.js 22+, and ESP-IDF 6.0.2 for firmware builds.
Clone the repository and install the per-user LaunchAgent:

```bash
git clone git@github.com:maxime-filippini/bird-remote.git
cd bird-remote
./tools/install-bird-server-macos
```

The installer:

- detects the MacBook's current Wi-Fi address;
- generates a shared token outside Git;
- writes matching local firmware settings;
- installs and starts `com.birdremote.server`;
- installs `~/bin/bird-flash`.

Reserve the reported address for the MacBook in the home router. Verify from a
phone on the same Wi-Fi before flashing:

```text
http://<MACBOOK-LAN-IP>:8080/health
```

It must return `{"status":"ok"}`. Then open `http://localhost:8080/` on the
MacBook and mirror the browser to the TV.

### Server operation

```bash
launchctl kickstart -k gui/$UID/com.birdremote.server
launchctl print gui/$UID/com.birdremote.server
tail -f "$HOME/Library/Logs/Bird Remote/server.log"
```

Server state and its token are under
`~/Library/Application Support/Bird Remote/`. They are never committed.

## Firmware

Connect the ESP32 over USB and run:

```bash
~/bin/bird-flash
```

The command pulls Git with `--ff-only`, builds, flashes, and opens the serial
monitor. Use a full flash for the first server-pivot firmware so its partition
table is updated. Exit the monitor with `Ctrl+]`.

Saved Wi-Fi provisioning remains available only when credentials are absent or
five reconnect attempts fail. Connected operation displays only the pixel-art
**NEW BIRD!** button. Each press permits one four-second authenticated request;
the button is disabled while that request is in flight.

## Bird collection

The 39 curated JPEGs and Wikimedia attribution are in `apps/assets/birds/`.
JPEG files are authoritative: stale metadata is ignored, while an existing JPEG
without attribution prevents server startup. Delete a JPEG to remove it from
the display; do not remove attribution for retained files.

Images retain their individual licenses recorded in `metadata.json`. The MIT
license applies to the software, not the photographs.

## Development

```bash
cd apps/server && npm test
cd apps && idf.py build
```

See [`apps/README.md`](apps/README.md) for the HTTP interface, firmware details,
and hardware acceptance procedure.
