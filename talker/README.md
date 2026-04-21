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
- `--dest-mac AA:BB:CC:DD:EE:FF` (required) — receiver's MAC.
- `--source testtone|wav` — default `testtone` (1 kHz sine, −6 dBFS).
- `--file PATH` — WAV file when `--source wav`. M1 accepts PCM 48 kHz 2ch 24-bit only; the file loops.

Needs `CAP_NET_RAW` to open `AF_PACKET`; easiest path is `sudo`.

## What it does, exactly

- Hardcoded stream: ID `0x0001`, format `0x11` (PCM s24le-3), 2 channels, 48 kHz.
- Emits 1 packet per 125 µs tick from `timerfd`. The timer never retunes.
- Ethernet II frame, EtherType `0x88B5`, AoE header per [`docs/wire-format.md`](../docs/wire-format.md).
- `payload_count` is **6 samples per packet nominally, but varies under Mode C feedback** — the talker keeps a fractional sample accumulator driven by the latest FEEDBACK value and writes the integer part into each packet. This is how USB hosts drive async DACs; we extend the same scheme across Ethernet.
- Listens for FEEDBACK frames on EtherType `0x88B6` (a second raw socket). Clamps accepted rates to ±1000 ppm of nominal. Reverts to nominal rate if no feedback arrives for 5 s.
- Presentation-time field is `0` (no PTP in M1).
- `last-in-group` flag set on every packet.

## Stopping

`SIGINT` / `SIGTERM` trigger a clean shutdown with a summary line on stderr.
