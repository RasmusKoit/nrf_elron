# Elron companion (USB / BLE push)

Fetches a **week** of live Elron departures (Tallinn-Väike → Kohila by default, any
route configurable) and pushes them to the nRF52840 **"Elron Display"**. The board
persists them and shows when to **start walking** to catch the soonest reachable
train. When the board is plugged in it pushes over **USB**; otherwise over **BLE**.

## Where to run it

Windows, Linux (BlueZ), or macOS — anything with a real Bluetooth radio. USB-only
pushes work over the cable regardless.

## Setup (uv)

```bash
cd companion
uv sync
```

## Use

```bash
uv run elron_push.py                 # fetch a week + push once (USB if plugged, else BLE)
uv run elron_push.py --dry-run       # fetch + print only
uv run elron_push.py --serve         # stay running; auto-(re)send when the board
                                     #   appears or comes back after a reflash
uv run elron_push.py --list-stations # print every selectable station name
uv run elron_push.py --walk-min 20   # door-to-platform minutes (default 20)
uv run elron_push.py --bootloader    # reboot the board into the UF2 bootloader
```

Other flags: `--origin` / `--dest`, `--days N` (default 7), `--date YYYY-MM-DD`,
`--device-name`, `--force` (bypass the cache).

On Linux, if USB pushes stall, ModemManager may be holding `/dev/ttyACM*` —
`sudo systemctl mask ModemManager`, or just let it fall back to BLE.

## Settings (`elron-config.json`)

Set your defaults once in `elron-config.json` (next to the script) and both the
one-shot push *and* `--serve` use them — so the walk time sticks:

```json
{ "walk_min": 20, "origin": "Tallinn-Väike", "dest": "Kohila",
  "days": 7, "device_name": "Elron Display" }
```

CLI flags override the config for a single run. `walk_min` is your door-to-platform
time: the board skips trains you can't reach in time and counts down to when you
must leave for the soonest one you *can* catch. Run `--list-stations` for the ~150
valid station names (a misspelling suggests close matches).

## Always-on service

`--serve` already survives device reflashes (it runs on the PC) and re-pushes when
the board reappears (USB replug within ~3 s; BLE otherwise).

**Windows** — drop `elron-service.vbs` into your Startup folder:

```powershell
copy elron-service.vbs "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Startup\"
```

It launches `uv run elron_push.py --serve` hidden (no console window). It defaults to
`%USERPROFILE%\elron_companion`; edit the path inside the `.vbs` if yours differs.

**Linux** — install the systemd *user* unit (instructions in its header):

```bash
cp elron-companion.service ~/.config/systemd/user/
systemctl --user enable --now elron-companion.service
```

## Live data source

Elron's Ridango ticketing backend (region 64), no API key — the same data
elron.pilet.ee shows, so it's current:

```
GET  https://api.ridango.com/v2/64/intercity/originstops
PUT  https://api.ridango.com/v2/64/intercity/stopareas/trips/direct
     {"date","origin_stop_area_id","destination_stop_area_id","channel":"web"}
```

A descriptive `User-Agent` is sent and results are cached (trips 20 min, stops 24 h)
so repeated runs don't hammer the API. Departures are stored as absolute timestamps.

## How the board picks the train

For each train it computes *leave time = departure − walk_min*. The hero number is
the countdown to the soonest leave time you haven't missed (with a small grace, so
"GO NOW" shows briefly). It turns red under 5 minutes.
