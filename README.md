# Lucas-BLE-ESP32-Monitor

A Bluetooth monitor for **Lucas LiFePO4 leisure batteries**. An ESP32-WROOM
polls each pack in turn over BLE and serves a dashboard on your local network.

**Tested only with the Lucas LLX100** (12 V 100 Ah, Group 31 case). Other packs
in the range are likely to use the same BMS and BLE module, but none have been
tried — if you have one, the Devices tab will tell you quickly whether it
answers.

This is an unofficial, independent project. It is not affiliated with,
endorsed by, or supported by Lucas or any battery manufacturer, and it comes
with no warranty. See [LICENSE](LICENSE).


## What it does

* Polls up to 8 packs round-robin over a single BLE radio.
* Reads pack voltage, current, power, state of charge, per-cell voltages, cell
  balance, temperatures, cycle count and estimated runtime.
* Charts voltage, current and power over time, with a **15m / 1h / 6h / 24h**
  range picker per chart.
* Keeps 24 hours of history on flash, so it survives a reboot.
* Configured entirely from the web UI — add packs by scanning for them, set the
  poll interval, update the firmware.
* No cloud, no account, no external requests. The dashboard is served from the
  board and works on an isolated network.

## Hardware

* An ESP32-WROOM development board (any ESP32 with 4 MB flash).
* One or more Lucas LLX100 packs, or another pack whose BMS speaks the same
  protocol over a BLE serial service — see
  [docs/protocol.md](docs/protocol.md) for exactly what the firmware expects.

Nothing is wired to the pack — the ESP32 talks to it over the air, the same way
a phone app would.

## Install

### From a browser, no toolchain

Every push to `main` publishes a [release](../../releases) with prebuilt images.

1. Download **`Lucas-BLE-ESP32-Monitor-<version>-full.bin`** from the latest
   release.
2. Open <https://esptool.spacehuhn.com/> in Chrome or Edge. It needs WebSerial,
   so Firefox and Safari will not work.
3. Connect the board over USB and press **Connect**.
4. Add the file at offset **`0x0`**, then **Program**.

One file, one offset — it already contains the bootloader, the partition table,
the OTA selector and the app.

**A full flash clears saved settings.** That image spans the NVS region, so the
saved WiFi network, device list and poll interval are erased and the board comes
back offering its `lucas-setup` access point. To update without losing them, use
the `-app.bin` from the same release: upload it on the dashboard's **Firmware**
tab, or flash it over USB at offset `0x10000`.

### From source

[PlatformIO](https://platformio.org/) builds it:

```bash
pio run                                        # build
pio run -t upload --upload-port /dev/ttyUSB0   # flash over USB
pio device monitor                             # serial console, 115200
```

The dashboard page is compiled in: `tools/gen_page.py` runs before every build,
checks `web/dashboard.html` for errors and gzips it into a header the firmware
serves directly.

### Releases

`.github/workflows/release.yml` builds on every push and pull request, and on
pushes to `main` also tags and publishes a release (`v0.1.<run number>`) with:

| Artifact | Flash at | Use |
|---|---|---|
| `...-full.bin` | `0x0` | first install or recovery; clears saved settings |
| `...-app.bin` | `0x10000`, or OTA | update, keeping WiFi and devices |
| `SHA256SUMS` | | checksums for both |

A pull request build produces the same images as a downloadable artifact but
publishes nothing.

## First run

1. Power the board. With no network saved it raises a WiFi access point called
   **`lucas-setup`** (password `lucaslucas` — change it in `include/config.h`).
2. Join that network from a phone or laptop. A captive portal opens; if it does
   not, browse to `http://192.168.4.1/`.
3. Pick your network and enter its password. The board reboots onto it and
   prints its address on the serial console.
4. Open `http://lucas.local/`, or the IP it printed.
5. On the **Devices** tab, press *Scan for devices*, and add your packs. Devices
   advertising a serial-over-BLE service are listed first.

To move the board to a different network later, run `wifi portal` on the serial
console.

## The dashboard

Three tabs:

* **Dashboard** — voltage, current and power over time, one chart each, with a
  range picker on every card. Below them, a card per pack: state of charge, the
  live figures, per-cell voltages with the highest and lowest marked, and a
  balance indicator.
* **Devices** — scan, add, remove and rename packs, and set the poll interval.
  Saved to NVS, so it survives reboots and firmware updates.
* **Firmware** — upload a new `.bin` over the air.

The page is self-contained: hand-written CSS and inline SVG charts, no CDN and
no external fetches, served gzipped (29 KB → 10 KB) straight from flash.

### History

Two tiers, so a 24-hour view does not cost one point per poll:

| | Where | Resolution | Depth |
|---|---|---|---|
| Live | RAM ring, `src/fleet.cpp` | one per poll | 180 samples |
| Archive | LittleFS, `src/histstore.cpp` | one averaged record a minute | 1440 records (24 h) |

Ranges up to 30 minutes are served from RAM at full poll resolution; longer
ranges come from the archive. Either way the series is decimated to at most 240
points before it leaves the board. The archive is one fixed-size circular file
per pack (~23 KB), so it never grows and never needs pruning; removing a pack
deletes its file.

Timestamps are **device-seconds carried across reboots in NVS**, not wall clock:
there is no RTC, and no guarantee of an internet connection for NTP. A counter
that only ever goes forwards cannot produce the backwards jumps an unsynced
clock would. The trade-off is that time does not advance while the board is off,
so a power cut leaves a discontinuity rather than a gap — the charts detect a
jump of more than 3.5× the typical sample spacing and break the line instead of
drawing through missing time.

## Updating over the air

From the Firmware tab, or from a shell:

```bash
curl -F firmware=@.pio/build/esp32dev/firmware.bin http://lucas.local/update
```

`pio run -e ota -t upload` also works.

**Changing `partitions.csv` requires a USB flash** — an OTA update rewrites only
the app image, never the partition table.

## Serial console

Configuration lives in the web UI; the console is a small maintenance shell.

```
status                   packs, poll counters and the dashboard URL
scan [secs]              list nearby BLE devices (pauses polling)
packs                    the saved device list
fleet start | stop       pause or resume polling
fleet <secs>             seconds per polling cycle
wifi                     network status and the dashboard URL
wifi portal              raise the WiFi setup access point again
wifi forget              erase the saved network and reboot into setup
log on | log off         print every decoded frame as it arrives
reboot
```

## Layout

```
.github/workflows/     CI: builds every push, releases on main
platformio.ini         build config; two envs, USB and OTA
partitions.csv         two 1.625 MB app slots plus 640 KB of LittleFS
include/config.h       hostname, setup AP name and password
web/dashboard.html     the dashboard, gzipped into the firmware at build time
tools/gen_page.py      pre-build: checks and compresses the page
tools/smoke_page.js    evaluates the page's script against a DOM stub
src/blelink.cpp        BLE radio: discovery, connection, characteristics
src/lucas.cpp          frame building, reassembly, CRC and decoding
src/poller.cpp         request/response cycle for one pack
src/fleet.cpp          round-robin poller task and the live history ring
src/histstore.cpp      the LittleFS archive
src/packstore.cpp      saved device list and poll interval, in NVS
src/netcfg.cpp         WiFi onboarding portal and OTA
src/webui.cpp          HTTP server and JSON API
docs/protocol.md       the BMS protocol this firmware speaks
```

## Compatibility

The firmware looks for a BLE serial service (`0xFFE0`/`0xFFF0`) and then speaks
the framed protocol documented in [docs/protocol.md](docs/protocol.md). A pack
that advertises one of those services is flagged in the device picker, but that
is only a hint — adding it and watching the poll counters is the real test. If
frames come back with a valid CRC but decode to nonsense, the field offsets in
`src/lucas.cpp` are the place to look.

Confirmed working:

| Pack | Configuration | Notes |
|---|---|---|
| Lucas LLX100 | 12 V 100 Ah, 4S LiFePO4 | the only pack this has been tested against |

## Notes

* One BLE radio means one connection at a time. Each pack is connected, polled
  and released in turn; polling pauses while a scan runs.
* BLE and WiFi share the radio, so WiFi modem sleep must stay enabled — the
  ESP32 aborts at boot otherwise.
* Signal strength matters more than anything else for reliability. A pack at the
  edge of range will fail polls, leave gaps in its history, and slow the web UI,
  because a failed connection holds the radio until it times out. Check the
  poll counters on each card.

## Licence

MIT — see [LICENSE](LICENSE).
