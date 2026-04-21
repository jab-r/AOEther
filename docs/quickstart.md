# AOEther quickstart

Get AOEther playing real music in about 30 minutes.

AOEther is a two-binary system: a **talker** on whatever box has the music, a **receiver** on whatever box has the USB DAC. Music sources are bridged in via the Linux kernel's `snd-aloop` module — any program that can play to ALSA (that is, essentially any program on Linux) becomes a valid AOEther source.

This page is the fastest happy path. For specific source ecosystems see:

- [`recipe-roon.md`](recipe-roon.md) — Roon users
- [`recipe-upnp.md`](recipe-upnp.md) — UPnP / DLNA controllers (BubbleUPnP, Kazoo, mconnect)
- [`recipe-capture.md`](recipe-capture.md) — any desktop audio (Tidal, browser tabs, Spotify desktop, CLI players) via PipeWire / PulseAudio

## What you need

- A Raspberry Pi 4 or 5 with Raspberry Pi OS (Bookworm, 64-bit) — this is the **receiver**.
- Any UAC2-compliant USB DAC plugged into the Pi.
- A Linux machine on the same wired LAN — this is the **talker**. It's where the music source runs.
- Cat 6 between them (or a dumb gigabit switch).

## Step 1 — Build both binaries

On the talker box and the Pi:

```sh
sudo apt install build-essential libasound2-dev
git clone https://github.com/<your-org>/AOEther.git
cd AOEther
make
```

This produces `talker/build/talker` and `receiver/build/receiver`.

## Step 2 — Smoke-test with a 1 kHz tone

Find the DAC's ALSA name on the Pi:

```sh
aplay -L | grep -A1 '^hw:'
# e.g. hw:CARD=Dragonfly,DEV=0
```

Find the Pi's MAC as seen from its Ethernet port:

```sh
ip link show eth0 | awk '/ether/ {print $2}'
```

On the Pi (receiver):

```sh
sudo ./receiver/build/receiver \
    --iface eth0 \
    --dac hw:CARD=Dragonfly,DEV=0
```

On the talker box:

```sh
sudo ./talker/build/talker \
    --iface eno1 \
    --dest-mac <pi-mac> \
    --source testtone
```

You should hear a clean 1 kHz tone through the DAC. If you do, the wire plumbing, Mode C clock feedback, and USB output are all working end to end.

`Ctrl-C` on both sides to stop.

## Step 3 — Pick a music source

The test tone is just a diagnostic. For real music, pick the recipe that matches your setup:

| You use... | Recipe |
|---|---|
| Roon (any tier) | [`recipe-roon.md`](recipe-roon.md) |
| A UPnP / DLNA controller | [`recipe-upnp.md`](recipe-upnp.md) |
| Desktop audio (browser, Tidal/Spotify desktop apps, AirPlay, Spotify Connect via librespot) | [`recipe-capture.md`](recipe-capture.md) |

All three recipes share the same pattern: they install a daemon on the talker box that outputs to `hw:Loopback,0,N`, and they run `talker --source alsa --capture hw:Loopback,1,N` to pick it up. The talker and receiver commands above stay exactly the same; only the bridge daemon changes.

## Format lock

From M2, the wire accepts any of 44.1 / 48 / 88.2 / 96 / 176.4 / 192 kHz and 1..64 channels at 24-bit PCM, but **the talker and receiver must be started with matching `--channels` and `--rate`**, and the source daemon must produce exactly that format. Mismatches fail with a clear error on startup — AOEther never silently resamples. DSD arrives in M6.

Very-high-rate multichannel combinations (e.g., 12 ch × 384 kHz) exceed the 1500-byte MTU when packed at 8000 pps; the talker rejects such configurations at startup. Typical deployments (stereo up to 192 kHz, 5.1 / 7.1 / 7.1.4 up to 192 kHz, 16 ch at 48 kHz) fit comfortably.

## When it doesn't work

- **Receiver starts but no audio**: check the receiver's `rx` counter on shutdown. If it's nonzero, packets arrive but ALSA playback may be misconfigured — verify `aplay -D hw:CARD=...,DEV=0 /usr/share/sounds/alsa/Front_Center.wav` plays a voice cleanly first.
- **Receiver's `dropped` counter rising fast**: talker and receiver may not be on the same broadcast domain. L2 EtherType `0x88B5` doesn't cross routers or WiFi bridges. M1 is wired point-to-point or via a dumb switch only.
- **Frequent underruns**: raise the receiver's jitter buffer with `--latency-us 10000`. The default (5000 µs) is tight on busy Pis.
- **Talker says `Device or resource busy` on the capture device**: some other process is holding it. `fuser -v /dev/snd/*` will tell you who.
