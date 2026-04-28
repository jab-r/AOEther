# Merging Ravenna interop recipe (DXD + DoP)

This recipe walks through full Ravenna interop with **Merging Technologies** gear (Hapi MkII, NADAC, Anubis, Horus) end-to-end on AOEther's `--transport rtp` path.
M9 Phase A–C provided the AES67 baseline (PCM 48–192 kHz with SDP / SAP / PTPv2); Phase E adds **DXD** (352.8 / 384 kHz PCM) and **DoP** (DSD64 / 128 / 256 wrapped in L24 PCM) so the same path covers Merging's full feature set.

For the AES67 baseline (Neumann KH AES67, Genelec SAM via Dante-AES67, Dutch & Dutch 8c) see [`recipe-aes67.md`](recipe-aes67.md).
For the AVB/Milan path (L-Acoustics Creations, Mac CoreAudio sources) see [`recipe-milan.md`](recipe-milan.md).
The architecture rationale for keeping both first-class is [`atmos-network-choice.md`](atmos-network-choice.md).

## What works

| Source | Wire format | Carrier rate | Merging support | AOEther flags |
|---|---|---|---|---|
| PCM 48 kHz | L24 RTP | 48000 Hz | Yes | `--format pcm --rate 48000` |
| PCM 96 kHz | L24 RTP | 96000 Hz | Yes | `--format pcm --rate 96000` |
| **DXD** 352.8 kHz | L24 RTP | 352800 Hz | Yes | `--format pcm --rate 352800 --ptime 250` |
| **DXD** 384 kHz | L24 RTP | 384000 Hz | Yes | `--format pcm --rate 384000 --ptime 250` |
| **DSD64-DoP** | L24 RTP | 176400 Hz | Yes | `--format dsd64 --ptime 250` |
| **DSD128-DoP** | L24 RTP | 352800 Hz | Yes | `--format dsd128 --ptime 250` |
| **DSD256-DoP** | L24 RTP | 705600 Hz | Yes (Merging cap) | `--format dsd256 --ptime 250` |
| DSD512-DoP | L24 RTP | 1411200 Hz | **No** | `--format dsd512 --ptime 125` (out of spec) |

Merging gear caps at DSD256 over DoP — that's the Ravenna spec ceiling.
DSD512-DoP is supported by AOEther for non-Merging gear that handles 1.4112 MHz L24 carriers; it is explicitly out of AES67 / Ravenna spec.

## Hardware setup

Two-segment example: AOEther talker on Linux, Merging Hapi MkII as the listener, M4250 switch in **Dante / AES67 profile**.

```
+----------------+      M4250         +-----------------+      analog
| AOEther talker |----------------->  | Merging Hapi    |--------------->
| (Linux + ptp4l)|   1 GbE / PTPv2    | MkII (Ravenna)  |    XLR / TRS
+----------------+                    +-----------------+

PTPv2 grandmaster: typically the M4250's BC, or a dedicated Mellanox / Linux+I210 GM.
See docs/ptp-setup.md for the daemon recipe.
```

Reverse direction works the same way: Merging gear as RTP talker, AOEther receiver into a USB DSD DAC.
The two cases are symmetric in M9 Phase E.

## Talker → Merging (DXD)

```sh
sudo ./build/talker \
  --iface eno1 \
  --transport rtp \
  --dest-ip 239.10.20.30 \
  --rate 384000 \
  --format pcm \
  --channels 2 \
  --ptime 250 \
  --ptp \
  --announce-sap \
  --session-name "AOEther DXD"
```

The `--ptime 250` is required at DXD: 1 ms ptime at 384 kHz × 2 ch × L24 = 2304 B/packet which exceeds 1500 B MTU.
Merging gear accepts both 1 ms and 250 µs ptime.

In Merging's ANEMAN, the announced session appears under "Sources".
Drag it onto the Hapi MkII's input.
PTP must converge first — if the talker's `--ptp` is on but the Hapi shows "PTP not synced", check that both are in the same PTP domain (`--ptp-domain N` on the talker; ANEMAN exposes the domain in the device properties).

## Talker → Merging (DSD via DoP)

```sh
sudo ./build/talker \
  --iface eno1 \
  --transport rtp \
  --dest-ip 239.10.20.30 \
  --format dsd64 \
  --source dsf --file album.dsf \
  --channels 2 \
  --ptime 250 \
  --ptp \
  --announce-sap \
  --session-name "AOEther DSD64-DoP"
```

The talker reads native DSD bytes from the DSF file, runs the DoP encoder (`common/dop.c`), and emits L24 PCM at the carrier rate (176.4 kHz for DSD64).
SDP advertises `L24/176400/2` — DoP is not signaled at the SDP layer, the Hapi detects it from the marker pattern in the L24 stream's MSB.

For DSD256:

```sh
sudo ./build/talker ... --format dsd256 --ptime 250 ...
```

This emits L24 at 705.6 kHz.
At 705.6 kHz × 2 ch × 3 = 4234 B/ms, so 250 µs ptime (1059 B/packet) is required.

## Talker → Merging (multichannel DXD)

For 7.1.6 (14 ch) DXD content, M10's multi-stream split applies — DXD is just PCM at a high rate, the bundling logic is the same as the M10 baseline.

```sh
sudo ./build/talker \
  --iface eno1 \
  --transport rtp \
  --dest-ip 239.10.20.30 \
  --rate 384000 \
  --format pcm \
  --channels 14 \
  --channels-per-stream 8 \
  --ptime 125 \
  --ptp \
  --announce-sap
```

Two substreams are emitted: 8 ch at `239.10.20.30` and 6 ch at `239.10.20.31`.
SDP carries one bundled session with `a=group:LS 1 2`.
Merging gear handles bundled multi-`m=` SDPs natively in ANEMAN; both substreams arrive sample-aligned.

## Merging → AOEther (passthrough to DoP-capable USB DAC)

The default receiver path treats incoming DoP-encoded RTP as plain L24 PCM and writes it to ALSA at the carrier rate.
Any DoP-capable USB DSD DAC (Topping, Holo Audio, RME, Merging NADAC, Exasound, etc.) sees the marker pattern and switches to DSD internally.

Subscribe to the Merging output in ANEMAN, point it at AOEther's multicast group, then on the AOEther receiver host:

```sh
sudo ./build/receiver \
  --iface eth0 \
  --dac hw:CARD=DAC,DEV=0 \
  --transport rtp \
  --group 239.10.20.40 \
  --port 5004 \
  --format dsd64 \
  --channels 2
```

The receiver opens ALSA as `SND_PCM_FORMAT_S24_3LE` at 176400 Hz; the DAC receives the L24 stream, sees DoP markers, and outputs DSD audio.
No `--alsa-format` override is needed — the passthrough path forces `pcm_s24_3le` regardless of `--format`.

## Merging → AOEther (native DSD output via `--unwrap-dop`)

For DACs that prefer native DSD over DoP-as-PCM (some snd_usb_audio quirk-tables only enable DSD via `SND_PCM_FORMAT_DSD_U*`), pass `--unwrap-dop` and an appropriate `--alsa-format`:

```sh
sudo ./build/receiver \
  --iface eth0 \
  --dac hw:CARD=DAC,DEV=0 \
  --transport rtp \
  --group 239.10.20.40 \
  --format dsd64 \
  --channels 2 \
  --unwrap-dop \
  --alsa-format dsd_u16_be
```

The receiver runs the DoP decoder (`dop_decode`), recovers native DSD bytes, and feeds them through the existing DSD-to-ALSA repack.
ALSA opens at the matching DSD rate (176400 Hz × 2 bytes/frame for DSD_U16_BE = matches the DSD64 byte rate).

For DSD256 the same flag pattern works:

```sh
... --format dsd256 --unwrap-dop --alsa-format dsd_u32_be
```

`SND_PCM_FORMAT_DSD_U8`, `DSD_U16_LE`, `DSD_U16_BE`, `DSD_U32_LE`, `DSD_U32_BE` are all valid; pick what your DAC's `snd_usb_audio` quirk exposes.

## PTP setup notes

Merging gear runs PTPv2 default profile (not gPTP / 802.1AS), in domain 0 by default.
Configure `ptp4l` on the AOEther talker to match — see [`ptp-setup.md`](ptp-setup.md):

```sh
ptp4l -i eno1 -f /etc/linuxptp/default.cfg -m
phc2sys -s eno1 -O 0 -m
```

The M4250 in Dante/AES67 profile boundaries PTP cleanly; in mixed deployments where you also have Milan gear in another VLAN, keep them separated — gPTP and PTPv2 default are profile-incompatible at the switch.

## DSD512 (out of spec)

DSD512-over-DoP runs L24 at 1.4112 MHz which is beyond AES67 / Ravenna's defined rate set.
Merging gear refuses these sessions (and rightly so).
Some non-Merging gear handles it — for example specific ESS-based USB DACs paired with a custom Linux talker / receiver pair.

```sh
sudo ./build/talker --transport rtp --format dsd512 --ptime 125 ...
```

Document any specific DAC you've validated against here.
At 1411.2 kHz × 2 ch × 3 = 8467 B/ms; 250 µs (2117 B) still doesn't fit, so 125 µs ptime is mandatory.

## Verification checklist

- [ ] Talker `--sdp-only` at the chosen rate prints the expected `L24/<carrier>/<channels>` rtpmap.
- [ ] `tcpdump -i eno1 udp port 5004` captures show the chosen ptime cadence (250 µs = 4000 pps; 125 µs = 8000 pps).
- [ ] Wireshark on a captured DoP stream shows `0xFA` / `0x05` alternating in the most-significant byte of every 24-bit sample, identical across channels within a frame.
- [ ] PTP converges on both ends (`pmc -u -b 0 'GET CURRENT_DATA_SET'` shows the same grandmaster).
- [ ] Merging device subscribes successfully via ANEMAN, no audio dropout under sustained playback (≥ 1 hour soak).
- [ ] On the receiver passthrough path, `aplay -l` shows the DAC at the carrier rate; on `--unwrap-dop`, `aplay -l` shows the DSD format the DAC's quirk exposes.

## Out of scope

- **AVDECC / Milan control plane** — Merging uses ANEMAN over RTSP/HTTP for control, not AVDECC. AOEther's M7 AVDECC support is for Milan listeners; it has no effect on Ravenna interop.
- **Multichannel DSD via DoP** — supported in principle (the encoder is rate- and channel-count-general), but no consumer Ravenna gear consumes multichannel DSD streams. Multichannel DSD remains on Modes 1/3 with native-DSD format codes (`0x30..0x35`) into a single multichannel DSD DAC; see [`recipe-dsd.md`](recipe-dsd.md).
- **DSD1024+** — DoP carrier would need ≥ 2.8224 MHz L24, no consumer endpoint or transport supports it.
