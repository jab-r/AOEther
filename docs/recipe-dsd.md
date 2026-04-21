# Recipe: native DSD end-to-end

Native DSD transport via the AOE wrapper (Mode 1 / Mode 3). PCM and DoP paths are unchanged; this recipe focuses on what M6 adds.

For the wire-format byte layout see [`wire-format.md`](wire-format.md) §"Native DSD".

## What works in M6

- Talker accepts `--format dsd64 | dsd128 | dsd256 | dsd512` and emits raw DSD bits on the wire with format codes `0x30..0x33`.
- Receiver accepts the same `--format` values and opens ALSA at `SND_PCM_FORMAT_DSD_U8`, which is a 1:1 match for the wire format's per-byte channel interleaving and MSB-first bit order.
- Mode C clock discipline works at DSD byte rates with no code change — the talker's fractional accumulator and the receiver's `snd_pcm_delay()`-based rate estimator are rate-independent.
- Works over both L2 (`--transport l2`) and IP/UDP (`--transport ip`). AVTP (`--transport avtp`) does not carry DSD — AAF is PCM-only — and the talker and receiver both reject `avtp + dsd*` at startup.

## What does NOT work yet

- **Only `SND_PCM_FORMAT_DSD_U8`.** Many modern DACs advertise `SND_PCM_FORMAT_DSD_U32_BE` or `DSD_U16_LE` instead. These work fine with `snd_usb_audio` natively, but require a byte-reorder step in our receiver (4-byte- or 2-byte-granularity channel interleaving instead of 1-byte). That follow-up is tracked in the M6 PR.
- **Only synthesized silence.** The talker's built-in `--source dsdsilence` emits the DSD idle pattern (`0x69`) on every channel. On a real DAC this plays as silence, which is exactly what's needed to verify the wire path and ALSA format selection work. Playing a real DSD file requires a `.dsf` / `.dff` reader (deferred to M8 alongside DSD1024/2048 — per-DAC quirk testing concentrates there).
- **DoP mode is not wired up.** The wire format reserves codes `0x20..0x23` for DoP (PCM s24le-3 with 0x05 / 0xFA marker bytes at inflated rates), and talker/receiver framework would accept them, but the talker has no DoP encoder source yet. If a DAC works only through DoP and not native DSD, use PCM mode for now and wait for the DoP encoder.
- **DSD1024 / DSD2048 require packet splitting.** At DSD1024 the per-microframe byte count exceeds 1500-byte MTU for stereo and the talker will refuse the configuration. M8 adds packet splitting and the `last-in-group` reassembly path.

## Step 1 — smoke test: talker → receiver → DSD DAC

Pick a rate your DAC supports natively. DSD64 and DSD128 are near-universal; DSD256 is common on modern DACs; DSD512 needs a specifically capable DAC.

```sh
# Receiver
sudo ./build/receiver --iface eth0 \
                      --dac hw:CARD=D90,DEV=0 \
                      --format dsd64

# Talker
sudo ./build/talker --iface eno1 \
                    --dest-mac <receiver-iface-MAC> \
                    --format dsd64
```

Expected banner output on receiver:

```
receiver: transport=l2 iface=eth0 dac=hw:CARD=D90,DEV=0 fmt=dsd64 ch=2 rate=352800 latency_us=5000 feedback=on
```

Expected on talker:

```
talker: transport=l2 iface=eno1 ifindex=N
        src=... dst=...
        fmt=dsd64 ch=2 rate=352800 pps=8000 nominal_spp=44.10 max_spp=48 max_frame=...B feedback=on
```

On the DAC front panel / status LEDs you should see "DSD64" (or equivalent). Audible output is silence — that's correct for the `--source dsdsilence` synth source. What you're verifying:

1. DAC indicator lights up as DSD (not PCM).
2. `rx` counter on receiver rises at ~8000 per second.
3. `fb_sent` counter on receiver rises (Mode C active; talker has locked onto the DAC clock).
4. No xruns / underruns over a 5-minute run.

## Step 2 — if the DAC rejects the stream

The most common cause is the DAC not supporting `SND_PCM_FORMAT_DSD_U8`. Check what ALSA sees for your hardware:

```sh
cat /proc/asound/card0/pcm0p/sub0/hw_params  # while receiver is running
# or:
amixer -c 0 contents  # look for DSD format support
```

If `snd_usb_audio` quirks-table only exposes `DSD_U32_BE` or `DSD_U16_LE` for your DAC, AOEther's M6 `DSD_U8` path cannot drive it. Options:

- Use PCM mode (`--format pcm --rate 192000`) and accept PCM output until the reorder follow-up lands.
- Patch `receiver/src/receiver.c::parse_format` to select `SND_PCM_FORMAT_DSD_U32_BE` for your DAC **and** add a transpose-by-4 step before `snd_pcm_writei` (the in-PR follow-up covers this properly).

## Step 3 — playing real DSD content

Not supported in M6. The bridge-via-snd-aloop pattern used for PCM (`docs/recipe-roon.md`, `docs/recipe-capture.md`) won't work for DSD directly either, because `snd-aloop` is PCM-only.

For now, the test workflow is silence-in / silence-out to verify protocol correctness. Real DSD playback arrives in M8 together with DSD1024/2048, a DSF file reader, and the per-DAC quirk matrix in [`dacs.md`](dacs.md).

If you're impatient: HQPlayer's NAA remains the audiophile-grade path for DSD file playback today. AOEther's DSD differentiation is that it's open-source and the control plane can be extended (PTP, multichannel, AVDECC in M7).

## Step 4 — DSD + IP/UDP

The IP/UDP transport from M4 works transparently with DSD:

```sh
sudo ./build/receiver --iface eth0 --dac hw:... --transport ip --port 8805 --format dsd64
sudo ./build/talker   --iface eno1 --transport ip --dest-ip 10.0.0.42 --format dsd64
```

Packet cadence and MTU behavior are the same as L2 — DSD64 stereo fits in ~90-byte frames regardless of transport. DSCP EF is still applied on egress.

## Clock behavior under DSD

Mode C feedback still applies and behaves identically to PCM: the receiver samples `snd_pcm_delay()` every 20 ms, differentiates `frames_written − delay` to estimate consumption rate, and emits Q16.16 samples-per-ms. Under DSD the Q16.16 value is the DSD byte rate / 1000 (e.g., ~352.8 for DSD64). This is outside the "UAC2 HS feedback" legal range but AOEther's talker treats it as an opaque rate target, not as UAC2 data. The ±1000 ppm safety clamp and the 5-second stale-feedback revert still apply.

## Troubleshooting

**Receiver: `snd_pcm_set_params (ch=2 rate=352800 fmt=dsd64): Invalid argument`** — DAC doesn't advertise DSD_U8. See Step 2 above.

**Talker: `AVTP AAF does not carry DSD; use --transport l2 or ip with --format dsd*`** — correct; AAF is PCM-only. Switch transport.

**Receiver: `dropped` counter rises with `rx=0`** — frames are arriving but the format code in the header doesn't match CLI. Make sure both talker and receiver use the same `--format`.

**Receiver: `underruns` counter rises steadily** — DSD bandwidth at DSD512 stereo is ~22 Mbps; if the network or the DAC's USB path can't sustain that, underruns follow. Try a lower DSD rate or check for USB 2.0 hub contention (DSD512 requires a direct USB 2.0 HS connection, no hub in most cases).
