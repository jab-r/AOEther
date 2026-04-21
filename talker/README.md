# AOEther talker (M1)

Sends stereo PCM over raw Ethernet at 8000 pps.
See [`docs/design.md`](../docs/design.md) §"M1 detailed plan" for scope and rationale.

## Build

```sh
sudo apt install build-essential
make
```

Produces `build/talker`.

## Run

```sh
sudo ./build/talker --iface eno1 \
                    --dest-mac AA:BB:CC:DD:EE:FF \
                    --source testtone
```

Flags:

- `--iface IF` (required) — egress interface, e.g. `eno1`, `enp3s0`.
- `--transport l2|ip|avtp` — default `l2` (raw Ethernet, AOE wrapper). `ip` switches to UDP (Mode 3, M4). `avtp` emits IEEE 1722 AAF on EtherType `0x22F0` for Milan interop (Mode 2, M5); see [`docs/recipe-milan.md`](../docs/recipe-milan.md).
- `--dest-mac AA:BB:CC:DD:EE:FF` — receiver's MAC (required with `--transport l2` or `--transport avtp`; for AVTP this is typically the listener's stream destination MAC, often a Milan-range multicast like `91:E0:F0:00:01:00`).
- `--dest-ip IP` — destination IPv4 or IPv6 literal (required with `--transport ip`). Multicast groups (224.0.0.0/4 or ff00::/8) are auto-detected.
- `--port N` — UDP port (IP mode only, default 8805).
- `--source testtone|wav|alsa` — default `testtone` (1 kHz sine, −6 dBFS).
- `--file PATH` — WAV file when `--source wav`. Accepts PCM 24-bit at any of the supported channel counts and rates; the file loops.
- `--capture hw:CARD=...` — ALSA PCM name when `--source alsa`. Point at one half of a `snd-aloop` pair to receive from Roon/UPnP/PipeWire; see `docs/recipe-*.md`.
- `--channels N` — channel count (1..64, default 2). Receiver must match.
- `--rate HZ` — one of 44100, 48000, 88200, 96000, 176400, 192000 (default 48000). Receiver must match.

Needs `CAP_NET_RAW` to open `AF_PACKET` in L2 mode; easiest path is `sudo`. IP mode doesn't require root in principle (bind on ephemeral port is unprivileged), but if you want to bind to port 8805 < 1024 you'd need capabilities anyway — 8805 is fine without.

## What it does, exactly

- Stream ID `0x0001`, format `0x11` (PCM s24le-3). Channels and rate are runtime-configured via `--channels` and `--rate`; defaults match M1 (2 channels, 48 kHz).
- Emits 1 packet per 125 µs tick from `timerfd`. The timer never retunes.
- Ethernet II frame, EtherType `0x88B5`, AoE header per [`docs/wire-format.md`](../docs/wire-format.md). With `--transport avtp` the wrapper switches to IEEE 1722 AAF (24-byte header, samples big-endian on the wire) at EtherType `0x22F0`; Mode C feedback continues on `0x88B6` regardless.
- `payload_count` is **nominally `rate_hz / 8000` samples per packet but varies under Mode C feedback** — the talker keeps a fractional sample accumulator driven by the latest FEEDBACK value and writes the integer part into each packet. This is how USB hosts drive async DACs; we extend the same scheme across Ethernet.
- Listens for FEEDBACK frames on EtherType `0x88B6` (a second raw socket). Clamps accepted rates to ±1000 ppm of nominal. Reverts to nominal rate if no feedback arrives for 5 s.
- MTU check at startup: configurations whose worst-case packet exceeds 1500 bytes (very high multichannel × hi-res combinations) are rejected with a clear message. Packet splitting for those cases is deferred to a later milestone.
- Presentation-time field is `0` (no PTP yet; arrives in M3).
- `last-in-group` flag set on every packet.

## Stopping

`SIGINT` / `SIGTERM` trigger a clean shutdown with a summary line on stderr.
