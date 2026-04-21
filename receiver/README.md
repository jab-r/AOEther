# AOEther receiver (M1)

Reads AOEther frames from raw Ethernet and writes samples to a USB DAC via ALSA.
See [`docs/design.md`](../docs/design.md) §"M1 detailed plan".

## Build

```sh
sudo apt install build-essential libasound2-dev
make
```

Produces `build/receiver`.

## Run

Find your DAC:

```sh
aplay -L | grep -A1 '^hw:'
```

Example output: `hw:CARD=Dragonfly,DEV=0`.

Start the receiver:

```sh
sudo ./build/receiver --iface eth0 \
                      --dac hw:CARD=Dragonfly,DEV=0
```

Flags:

- `--iface IF`, `--dac hw:...` (required) — as above.
- `--transport l2|ip|avtp` — default `l2` (raw Ethernet, AOE wrapper). `ip` switches to UDP (Mode 3, M4). `avtp` accepts IEEE 1722 AAF on EtherType `0x22F0` (Mode 2, M5); see [`docs/recipe-milan.md`](../docs/recipe-milan.md).
- `--port N` — UDP port to bind (IP mode only, default 8805).
- `--group IP` — multicast group to join (IP mode only). IPv4 in 224.0.0.0/4 or IPv6 in ff00::/8. Omit for unicast.
- `--channels N` — channel count (1..64, default 2). Must match the talker.
- `--rate HZ` — one of 44100, 48000, 88200, 96000, 176400, 192000 (default 48000). Must match the talker.
- `--latency-us N` — ALSA period latency hint (default 5000 µs). Generous on purpose; the Mode C loop corrects ppm-scale drift slowly and the buffer also absorbs talker-side `timerfd` jitter.
- `--no-feedback` — disable FEEDBACK emission. **Diagnostic only** — the positive control for the soak test (design.md §M1 test 7): with feedback off, the stream is expected to drift and xrun within minutes, confirming Mode C is doing real work when it's on.

Needs `CAP_NET_RAW` for the raw sockets in L2 mode; easiest path is `sudo`. IP mode doesn't require root for port 8805.

## What it does, exactly

- Opens two `AF_PACKET` sockets: one for RX on EtherType `0x88B5` (data — or `0x22F0` with `--transport avtp`), one for TX on `0x88B6` (Mode C FEEDBACK).
- Accepts only data frames matching the M1 format: magic `0xA0`, version `0x01`, format `0x11` (PCM s24le-3), 2 channels.
- Opens the named ALSA device at `S24_3LE`, 2ch, 48 kHz, ALSA soft-resample disabled.
- Forwards payload bytes directly into `snd_pcm_writei`. ALSA is the jitter buffer; `snd_usb_audio` is the UAC2 stack and runs UAC2 async feedback with the DAC.
- On xrun (`EPIPE`) calls `snd_pcm_prepare`, re-seeds the rate estimator, and continues.
- Tracks sequence-number gaps for loss reporting; no reorder buffer in M1.
- **Mode C clock discipline**: samples `snd_pcm_delay()` every 20 ms, differentiates `frames_written − delay` to estimate the DAC's consumption rate, and emits a FEEDBACK frame to the talker containing that rate as Q16.16 samples/ms. See [`docs/design.md`](../docs/design.md) §"Clock architecture" for why this is the baseline operating mode rather than a Mode A open-loop setup.
- Talker MAC is learned from the Ethernet source address of the first data frame received; feedback emission starts after that.

## Stopping

`SIGINT` / `SIGTERM` trigger a clean shutdown with rx/dropped/lost/underrun counters on stderr.

## Troubleshooting

- **`snd_pcm_open ... Device or resource busy`** — another process (PulseAudio, PipeWire) has the DAC. Stop that service, or point PipeWire away from the card: `systemctl --user stop pipewire pipewire-pulse` while testing.
- **No audio, rx counter rising** — your DAC likely doesn't advertise `S24_3LE` at 48 kHz. For M1 we hardcode that format; use a DAC that supports it, or wait for M2 format negotiation.
- **ALSA `Broken pipe` spam** — underruns. Packets aren't arriving in time. Check `ethtool -S <iface>`, run on a wired link, raise `LATENCY_US` in `receiver.c`.
