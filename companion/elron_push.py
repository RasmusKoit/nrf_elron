#!/usr/bin/env python3
"""
Elron companion — push a week of LIVE Tallinn-Väike -> Kohila departures to the
nRF52840 "Elron Display" over BLE, as absolute timestamps so the board shows the
right next train across days with no re-sync.

Live source: Elron's Ridango backend (region 64), no API key. Same data as
elron.pilet.ee, so it's current (not a snapshot).

Runs on Windows, Linux (BlueZ), or macOS — any machine with a real Bluetooth
radio. USB push works the same everywhere (the board's data port is COMx on
Windows, /dev/ttyACM* on Linux/macOS).
    uv sync
    uv run elron_push.py                 # fetch a week + push once
    uv run elron_push.py --dry-run       # fetch + print only
    uv run elron_push.py --serve         # stay running; auto-(re)send to the
                                         #   board whenever it appears / reflashes
    uv run elron_push.py --list-stations # print every selectable station name
    uv run elron_push.py --bootloader    # reboot the board into the UF2 bootloader

The board shows when to *start walking* for the soonest catchable train; set your
door-to-platform time with --walk-min (default 20).
"""
from __future__ import annotations

import argparse
import asyncio
import datetime as dt
import json
import pathlib
import struct
import sys
import time
from zoneinfo import ZoneInfo

import requests

# ── Live data source (Ridango / Elron) ─────────────────────────────────────
RIDANGO_API = "https://api.ridango.com/v2"
REGION_ID = 64
TZ = ZoneInfo("Europe/Tallinn")

USER_AGENT = ("nrf-elron-display/1.0 (+https://github.com/RasmusKoit/nrf_elron; "
              "personal departure-board, low volume)")
TRIPS_TTL_S = 1200           # reuse a day's trips within 20 min
STOPS_TTL_S = 24 * 3600
CACHE_FILE = pathlib.Path.home() / ".cache" / "elron_companion.json"

_session = requests.Session()
_session.headers.update({"User-Agent": USER_AGENT,
                         "Accept": "application/json, text/plain, */*"})

# Custom GATT (must match src/ble_svc.c)
SCHED_CHAR_UUID = "6e656c45-726f-6e21-0000-000000000002"
CTRL_CHAR_UUID = "6e656c45-726f-6e21-0000-000000000003"
CMD_BOOTLOADER = 0xB0
MFG_COMPANY_ID = 0xFFFF       # advert manufacturer-data: [company][has_schedule]
CHUNK = 18
DEFAULT_DEVICE_NAME = "Elron Display"

WIRE_VERSION = 3
MAX_DEPARTURES = 120
NAME_MAX = 15
DESTNAME_MAX = 11
MSG_MAX = 39
MAX_DESTS = 10
DEFAULT_WALK_MIN = 20

_TRANSLIT = str.maketrans(
    {"ä": "a", "ö": "o", "õ": "o", "ü": "u", "š": "s", "ž": "z",
     "Ä": "A", "Ö": "O", "Õ": "O", "Ü": "U", "Š": "S", "Ž": "Z"})


def ascii_name(s: str) -> str:
    return s.translate(_TRANSLIT).encode("ascii", "ignore").decode()[:NAME_MAX]


# ── Cache ──────────────────────────────────────────────────────────────────
def _cache_load() -> dict:
    try:
        return json.loads(CACHE_FILE.read_text())
    except Exception:
        return {}


def _cache_save(c: dict) -> None:
    try:
        CACHE_FILE.parent.mkdir(parents=True, exist_ok=True)
        CACHE_FILE.write_text(json.dumps(c))
    except Exception:
        pass


# ── Ridango live API ───────────────────────────────────────────────────────
def _load_stops(cache: dict, now_s: float, force: bool) -> list:
    """The Ridango list of selectable stations (cached 24h)."""
    sc = cache.get("stops")
    if sc and not force and now_s - sc["ts"] < STOPS_TTL_S:
        return sc["data"]
    r = _session.get(f"{RIDANGO_API}/{REGION_ID}/intercity/originstops", timeout=30)
    r.raise_for_status()
    stops = r.json()
    cache["stops"] = {"ts": now_s, "data": stops}
    return stops


def station_names(force: bool = False) -> list[str]:
    """Sorted list of all configurable Elron station names."""
    cache = _cache_load()
    stops = _load_stops(cache, time.time(), force)
    _cache_save(cache)
    return sorted({s.get("stop_name", "").strip()
                   for s in stops if s.get("stop_name", "").strip()})


def _stop_ids(origin: str, dest: str, cache: dict, now_s: float, force: bool):
    import difflib
    stops = _load_stops(cache, now_s, force)
    by_name = {s["stop_name"].strip(): s["stop_area_id"]
               for s in stops if s.get("stop_name", "").strip()}
    fold = {k.casefold(): v for k, v in by_name.items()}

    def find(name):
        v = fold.get(name.strip().casefold())
        if v is not None:
            return v
        near = difflib.get_close_matches(name, list(by_name), n=5, cutoff=0.4)
        hint = (" Did you mean: " + ", ".join(near) + "?") if near else \
               " Run with --list-stations to see valid names."
        raise SystemExit(f"[api] station '{name}' not found.{hint}")

    return find(origin), find(dest)


def _fetch_day(oid: str, did: str, day: dt.date, cache: dict, now_s: float, force: bool):
    key = f"trips:{day.isoformat()}:{oid}:{did}"
    tc = cache.get(key)
    if tc and not force and now_s - tc["ts"] < TRIPS_TTL_S:
        return tc["data"], True
    body = {"date": day.isoformat(), "origin_stop_area_id": oid,
            "destination_stop_area_id": did, "channel": "web"}
    r = _session.put(f"{RIDANGO_API}/{REGION_ID}/intercity/stopareas/trips/direct",
                     json=body, timeout=30)
    r.raise_for_status()
    cache[key] = {"ts": now_s, "data": r.json()}
    return cache[key]["data"], False


def fetch_week(origin: str, dest: str, start: dt.date, days: int, force: bool):
    """Return (sorted-by-epoch list of departure dicts, disruption msg)."""
    cache = _cache_load()
    now_s = time.time()
    oid, did = _stop_ids(origin, dest, cache, now_s, force)

    by_epoch: dict[int, dict] = {}
    msgs: list[str] = []
    for n in range(days):
        day = start + dt.timedelta(days=n)
        data, _ = _fetch_day(oid, did, day, cache, now_s, force)
        for m in data.get("disruption_messages", []):
            t = (m.get("title") or m.get("text") or m.get("message") or "").strip()
            if t:
                msgs.append(t)
        for journey in data.get("journeys", []):
            for t in journey.get("trips", []):
                dep = dt.datetime.fromisoformat(t["departure_time"])
                if dep.astimezone(TZ).date() != day:
                    continue
                epoch = int(dep.timestamp())
                dmin = t.get("departure_time_min", 0) or 0
                amin = t.get("arrival_time_min", dmin) or dmin
                dur = (amin - dmin) % 1440
                head = (t.get("trip_headsign") or "").split()
                final = head[-1] if head else (t.get("route_short_name") or "")
                by_epoch[epoch] = {
                    "epoch": epoch, "dur": min(dur, 255), "dest": final,
                    "express": bool(t.get("fast_train")),
                    "hhmm": dep.astimezone(TZ).strftime("%a %H:%M"),
                }
    _cache_save(cache)
    deps = [by_epoch[k] for k in sorted(by_epoch)]
    return deps, " | ".join(dict.fromkeys(msgs))


# ── Wire packing (v3) ──────────────────────────────────────────────────────
def _lp(s: str) -> bytes:
    b = s.encode("ascii", "ignore")
    return struct.pack("<B", len(b)) + b


def pack_payload(origin, dest, deps, msg, walk_min, when):
    epoch_now = int(when.timestamp())
    tz_off = int(when.utcoffset().total_seconds() // 60)

    # Only upcoming departures, capped.
    upcoming = [d for d in deps if d["epoch"] >= epoch_now - 60][:MAX_DEPARTURES]

    table: list[str] = []
    for d in upcoming:
        dn = ascii_name(d["dest"])[:DESTNAME_MAX]
        d["_dn"] = dn
        if dn and dn not in table and len(table) < MAX_DESTS:
            table.append(dn)

    buf = bytearray()
    buf += struct.pack("<B", WIRE_VERSION)
    buf += struct.pack("<I", epoch_now)
    buf += struct.pack("<h", tz_off)
    buf += struct.pack("<H", walk_min)
    buf += _lp(ascii_name(origin))
    buf += _lp(ascii_name(dest))
    buf += _lp(ascii_name(msg)[:MSG_MAX])
    buf += struct.pack("<B", len(table))
    for name in table:
        buf += _lp(name)
    buf += struct.pack("<B", len(upcoming))
    for d in upcoming:
        idx = table.index(d["_dn"]) if d["_dn"] in table else 0
        flags = 0x01 if d["express"] else 0x00
        buf += struct.pack("<IBBB", d["epoch"], d["dur"], idx, flags)
    return bytes(buf), len(upcoming)


# ── BLE ────────────────────────────────────────────────────────────────────
def _ble_kwargs() -> dict:
    """Backend-specific BleakClient kwargs. On Windows, bypass WinRT's stale GATT
    cache; on Linux (BlueZ) / macOS that kwarg doesn't apply, so pass nothing."""
    if sys.platform == "win32":
        return dict(winrt=dict(use_cached_services=False))
    return {}


async def _find(device_name, timeout=12.0):
    from bleak import BleakScanner
    return await BleakScanner.find_device_by_name(device_name, timeout=timeout)


async def push_ble(device_name: str, payload: bytes, dev=None) -> None:
    from bleak import BleakClient
    if dev is None:
        dev = await _find(device_name)
    if dev is None:
        raise RuntimeError(f"device \"{device_name}\" not found")
    async with BleakClient(dev, **_ble_kwargs()) as client:
        n = (len(payload) + CHUNK - 1) // CHUNK
        for seq in range(n):
            frame = bytes([seq]) + payload[seq * CHUNK:(seq + 1) * CHUNK]
            await client.write_gatt_char(SCHED_CHAR_UUID, frame, response=True)
        print(f"[ble] pushed {len(payload)} bytes in {n} frames")


async def push_with_retry(device_name: str, payload: bytes, tries: int = 5) -> None:
    """One-shot push that tolerates the board being briefly busy/advertising-late."""
    for i in range(tries):
        try:
            await push_ble(device_name, payload)
            return
        except Exception as e:
            print(f"[ble] attempt {i + 1}/{tries} failed: {e}")
            if i + 1 < tries:
                await asyncio.sleep(8)
    raise SystemExit("[ble] gave up — is the board powered and in range?")


# ── USB serial (preferred when plugged in — more reliable than BLE) ──────────
# The board exposes TWO CDC ACM ports sharing VID:PID 2FE3:0100: a log console
# and a data channel. Windows reorders/duplicates these COM assignments across
# reboots, so we DON'T guess by interface number — we ping each candidate and
# use the one that ACKs (only the data port answers). The board also ACKs after
# it actually applies a pushed schedule, so a push is confirmed end-to-end.
SERIAL_VID = 0x2FE3
SERIAL_PID = 0x0100
FRAME_DATA = 0x45
FRAME_PING = 0x50
SERIAL_ACK = 0x06
DATA_PING = bytes([0x01, FRAME_PING])

_data_port_cache = None


def _candidate_ports():
    from serial.tools import list_ports
    return [p.device for p in list_ports.comports()
            if (p.vid == SERIAL_VID and p.pid == SERIAL_PID)
            or (p.product and "Elron Display" in p.product)]


def _port_acks(dev) -> bool:
    """True if this COM port is the board's data channel (answers a ping)."""
    import serial
    try:
        with serial.Serial(dev, 115200, timeout=0.8, write_timeout=2) as s:
            s.reset_input_buffer()
            s.write(DATA_PING)
            s.flush()
            r = s.read(1)
            return len(r) == 1 and r[0] == SERIAL_ACK
    except Exception:
        return False


def find_com_port():
    """The board's data COM port, discovered by ACK (None if not present).
    Caches the winner and re-validates it first for speed."""
    global _data_port_cache
    cands = _candidate_ports()
    if not cands:
        _data_port_cache = None
        return None
    if _data_port_cache in cands and _port_acks(_data_port_cache):
        return _data_port_cache
    for dev in cands:
        if _port_acks(dev):
            _data_port_cache = dev
            return dev
    return None


def push_serial(payload: bytes) -> bool:
    """Push the wire payload over the board's data USB port. Returns True only if
    the board confirms it applied (ACK), so a silent failure falls back to BLE.
    Opens at 115200 (NOT 1200 — that would trigger the bootloader)."""
    import serial
    port = find_com_port()
    if not port:
        return False
    frame = bytes([0x01, FRAME_DATA]) + struct.pack("<H", len(payload)) + payload
    with serial.Serial(port, 115200, timeout=3, write_timeout=3) as s:
        s.reset_input_buffer()
        s.write(frame)
        s.flush()
        ack = s.read(1)   # board ACKs after it applies + persists the schedule
    acked = len(ack) == 1 and ack[0] == SERIAL_ACK
    if acked:
        print(f"[serial] pushed {len(payload)} bytes via {port} (apply confirmed)")
    else:
        print(f"[serial] pushed via {port} but no apply-ACK — will try BLE")
    return acked


async def reboot_to_bootloader(device_name: str) -> None:
    from bleak import BleakClient
    dev = await _find(device_name)
    if dev is None:
        raise SystemExit(f"[ble] device \"{device_name}\" not found.")
    async with BleakClient(dev, **_ble_kwargs()) as client:
        try:
            await client.write_gatt_char(CTRL_CHAR_UUID, bytes([CMD_BOOTLOADER]),
                                         response=True)
        except Exception:
            pass
    print("[ble] bootloader reboot command sent")


async def scan_state(device_name: str):
    """Return (device, has_schedule) if advertising, else None."""
    from bleak import BleakScanner
    found = await BleakScanner.discover(timeout=6.0, return_adv=True)
    for _addr, (dev, adv) in found.items():
        if (adv.local_name or dev.name) == device_name:
            md = adv.manufacturer_data.get(MFG_COMPANY_ID)
            has = md[0] if md else None
            return dev, has
    return None


async def serve(origin, dest, device_name, walk_min, days):
    """Stay running: refresh data hourly, and (re)push whenever the board shows
    up empty (just reflashed/plugged in) or our data is newer than the last push.

    Replug is detected fast: USB presence is a cheap port-list check done every
    POLL_S, so plugging the board back in re-syncs within a few seconds. The
    heavier BLE scan (only needed when it's NOT on USB) is throttled to BLE_SCAN_S
    so it doesn't slow that poll down."""
    REFRESH_S = 3600        # re-fetch the week hourly
    POLL_S = 3              # USB presence poll (cheap; sets replug latency)
    BLE_SCAN_S = 45         # how often to fall back to a BLE scan when off USB
    deps, msg, last_fetch, last_push, last_ble_scan = None, "", 0.0, 0.0, 0.0
    com_present = False
    print("[serve] watching for the board; USB replug re-syncs within a few seconds")
    while True:
        now = time.time()
        if deps is None or now - last_fetch > REFRESH_S:
            try:
                deps, msg = fetch_week(origin, dest, dt.datetime.now(TZ).date(),
                                       days, force=(deps is not None))
                last_fetch = now
                print(f"[serve] data refreshed: {len(deps)} departures")
            except Exception as e:
                print(f"[serve] fetch failed: {e}")
                await asyncio.sleep(30)
                continue

        def payload():
            return pack_payload(origin, dest, deps, msg, walk_min, dt.datetime.now(TZ))

        # Cheap presence check (lists COM ports, doesn't open them).
        present = bool(_candidate_ports())
        if present:
            appeared = not com_present
            com_present = True
            # Push when the board just appeared (plugged in / rebooted) or our
            # data is newer than the last push. push_serial pings to find the
            # data port and only succeeds on the board's apply-ACK.
            if appeared or last_push < last_fetch:
                try:
                    pl, n = payload()
                    if push_serial(pl):
                        last_push = time.time()
                        print(f"[serve] sent {n} departures via USB "
                              f"({'plugged in' if appeared else 'refresh'})")
                except Exception as e:
                    print(f"[serve] serial push failed: {e}")
        else:
            if com_present:
                print("[serve] board unplugged — watching for replug / BLE")
            com_present = False
            # Wireless fallback: BLE, using the advertised needs-sync flag. The
            # scan is slow (~6 s), so throttle it; the USB poll stays snappy.
            if now - last_ble_scan >= BLE_SCAN_S:
                last_ble_scan = now
                try:
                    st = await scan_state(device_name)
                except Exception as e:
                    print(f"[serve] scan error: {e}")
                    st = None
                if st is not None:
                    dev, has = st
                    if has == 0 or last_push < last_fetch:
                        try:
                            pl, n = payload()
                            await push_ble(device_name, pl, dev=dev)
                            last_push = time.time()
                            print(f"[serve] sent {n} departures via BLE "
                                  f"({'empty board' if has == 0 else 'refresh'})")
                        except Exception as e:
                            print(f"[serve] BLE push failed: {e}")
        await asyncio.sleep(POLL_S)


def load_config() -> dict:
    """Defaults from elron-config.json next to this script (CLI flags override).
    Lets you set walk_min/route once for both one-shot and --serve."""
    try:
        p = pathlib.Path(__file__).parent / "elron-config.json"
        return json.loads(p.read_text(encoding="utf-8"))
    except Exception:
        return {}


# ── CLI ────────────────────────────────────────────────────────────────────
def main() -> None:
    cfg = load_config()
    ap = argparse.ArgumentParser(description="Push live Elron departures to the BLE display")
    ap.add_argument("--origin", default=cfg.get("origin", "Tallinn-Väike"))
    ap.add_argument("--dest", default=cfg.get("dest", "Kohila"))
    ap.add_argument("--device-name", default=cfg.get("device_name", DEFAULT_DEVICE_NAME))
    ap.add_argument("--walk-min", type=int, default=cfg.get("walk_min", DEFAULT_WALK_MIN),
                    help="minutes to walk to the station (config: walk_min, default 20)")
    ap.add_argument("--days", type=int, default=cfg.get("days", 7),
                    help="days of schedule to load")
    ap.add_argument("--date", default=None, help="start date YYYY-MM-DD (default today)")
    ap.add_argument("--dry-run", action="store_true", help="print, don't push")
    ap.add_argument("--force", action="store_true", help="bypass the cache")
    ap.add_argument("--serve", action="store_true",
                    help="stay running and auto-(re)send when the board appears")
    ap.add_argument("--bootloader", action="store_true",
                    help="reboot the board into the UF2 bootloader, then exit")
    ap.add_argument("--list-stations", action="store_true",
                    help="print every selectable station name and exit "
                         "(use these for --origin/--dest or elron-config.json)")
    args = ap.parse_args()

    if args.list_stations:
        names = station_names(force=args.force)
        print(f"{len(names)} Elron stations (use for --origin / --dest):")
        for n in names:
            print(f"  {n}")
        return
    if args.bootloader:
        asyncio.run(reboot_to_bootloader(args.device_name))
        return
    if args.serve:
        asyncio.run(serve(args.origin, args.dest, args.device_name,
                          args.walk_min, args.days))
        return

    now = dt.datetime.now(TZ)
    start = dt.date.fromisoformat(args.date) if args.date else now.date()
    deps, msg = fetch_week(args.origin, args.dest, start, args.days, force=args.force)
    if not deps:
        raise SystemExit(f"[api] no {args.origin} -> {args.dest} departures")

    print(f"\n{args.origin} -> {args.dest}  ({len(deps)} departures over {args.days}d, "
          f"walk {args.walk_min}m):")
    if msg:
        print(f"  ! {msg}")
    for d in deps[:20]:
        exp = " [express]" if d["express"] else ""
        print(f"  {d['hhmm']} -> {d['dest']}{exp}")
    if len(deps) > 20:
        print(f"  ... +{len(deps) - 20} more")

    payload, n = pack_payload(args.origin, args.dest, deps, msg, args.walk_min, now)
    print(f"\nwire payload: {len(payload)} bytes, {n} upcoming departures")

    if args.dry_run:
        print("[dry-run] not pushing")
        return
    # Prefer USB serial (reliable when plugged in); fall back to BLE.
    try:
        if push_serial(payload):
            print("done.")
            return
    except Exception as e:
        print(f"[serial] failed ({e}); trying BLE")
    asyncio.run(push_with_retry(args.device_name, payload))
    print("done.")


if __name__ == "__main__":
    main()
