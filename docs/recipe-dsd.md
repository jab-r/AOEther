# Recipe: native DSD end-to-end

Native DSD transport via the AOE wrapper (Mode 1 / Mode 3). PCM and DoP paths are unchanged.

For the wire-format byte layout see [`wire-format.md`](wire-format.md) §"Native DSD"; for per-microframe packet splitting (used by DSD512 and higher) see §"Cadence and fragmentation".

## What works

- Talker accepts `--format dsd64 | dsd128 | dsd256 | dsd512 | dsd1024 | dsd2048` and emits raw DSD bits on the wire with format codes `0x30..0x35`. DSD512 and higher are carried via per-microframe packet splitting — at stereo that's 2 fragments per microframe for DSD512, 3 for DSD1024, 6 for DSD2048 — with no receiver-side reassembly state (each fragment is a complete AoE data frame). Receiver-side ALSA support for these rates depends on the DAC's `snd_usb_audio` quirk entry; most current DSD1024 DACs are already wired up in recent Linux kernels.
- Receiver accepts the same `--format` values plus an `--alsa-format` override to match the ALSA DSD format your DAC's `snd_usb_audio` quirk exposes:
  - `dsd_u8` (default) — wire bytes pass through 1:1, zero reorder.
  - `dsd_u16_le` / `dsd_u16_be` — receiver deinterleaves into per-channel streams and repacks at 2-byte granularity, byte-reversing within each 2-byte group for `_le`.
  - `dsd_u32_le` / `dsd_u32_be` — same at 4-byte granularity.
  - The receiver carries up to (N−1) bytes per channel across packet boundaries, so the talker's fractional accumulator can emit any `payload_count` without worrying about ALSA-frame alignment.
- Mode C clock discipline works at DSD byte rates with no code change — the talker's fractional accumulator and the receiver's `snd_pcm_delay()`-based rate estimator are rate-independent.
- Works over both L2 (`--transport l2`) and IP/UDP (`--transport ip`). AVTP (`--transport avtp`) does not carry DSD — AAF is PCM-only — and the talker and receiver both reject `avtp + dsd*` at startup.

## What does NOT work yet

- **DFF (.dff) file reading.** AOEther ships a DSF reader (Sony `.dsf`) but not a DSF-Interchange-File-Format reader. DFF is a straightforward follow-up — its bit order already matches the AOE wire format (MSB-first) — but is deferred alongside the per-DAC quirk matrix.
- **DoP mode is not wired up.** The wire format reserves codes `0x20..0x23` for DoP (PCM s24le-3 with 0x05 / 0xFA marker bytes at inflated rates), and talker/receiver framework would accept them, but the talker has no DoP encoder source yet. If a DAC works only through DoP and not native DSD, use PCM mode for now and wait for the DoP encoder.

## Step 1 — smoke test: talker → receiver → DSD DAC

Pick a rate your DAC supports natively. DSD64 and DSD128 are near-universal; DSD256 is common on modern DACs. DSD512 is supported on many modern DACs but requires packet splitting on our side, landing in M8.

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
receiver: transport=l2 iface=eth0 dac=hw:CARD=D90,DEV=0 fmt=dsd64 alsa=dsd_u8 ch=2 rate=352800 alsa_rate=352800 latency_us=5000 feedback=on
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

## Step 2 — picking the right `--alsa-format` for your DAC

If the default (`dsd_u8`) open fails with `Invalid argument`, the DAC's `snd_usb_audio` quirk probably exposes only a wider DSD format. Check what ALSA says:

```sh
cat /proc/asound/card0/pcm0p/sub0/hw_params  # while receiver is running
# or:
cat /proc/asound/card0/stream0  # shows the advertised UAC/DSD formats
```

Look for a line like `Format: DSD_U32_BE` or `DSD_U16_LE`. Then restart the receiver with a matching override:

```sh
sudo ./build/receiver --iface eth0 --dac hw:CARD=D90,DEV=0 \
                      --format dsd64 --alsa-format dsd_u32_be
```

The receiver deinterleaves the wire bytes into per-channel streams, repacks them at the right granularity (2 or 4 bytes per channel per ALSA frame), and reverses byte order within each group for `_le` variants. Any leftover bytes below one ALSA-frame's worth are carried over to the next packet, so payload alignment on the wire is not a concern.

Common quirk → flag mapping:

| DAC family                    | Typical ALSA format | Flag              |
|-------------------------------|---------------------|-------------------|
| Topping D90 / E70 / A90D      | DSD_U32_BE          | `dsd_u32_be`      |
| SMSL M500 / M400              | DSD_U32_BE          | `dsd_u32_be`      |
| RME ADI-2 DAC                 | DSD_U32_BE          | `dsd_u32_be`      |
| Holo May / Spring / Cyan      | DSD_U32_BE          | `dsd_u32_be`      |
| Older iFi / Chord             | DSD_U16_LE          | `dsd_u16_le`      |
| DACs with pure DSD-UAC support | DSD_U8             | `dsd_u8` (default) |

These are typical — trust `/proc/asound` over this table.

## Step 3 — playing real DSD content

The talker accepts Sony DSF (`.dsf`) files via `--source dsf --file PATH.dsf`. Point `--format` at the file's DSD rate and `--channels` at its channel count; the file is validated at startup and loops when it hits EOF (same semantics as the WAV source).

```sh
# DSD64 stereo .dsf, Mode 1 transport:
sudo ./build/talker --iface eno1 --dest-mac <receiver-iface-MAC> \
                    --source dsf --file track.dsf \
                    --format dsd64 --channels 2
```

What the reader does internally:

- Parses the DSF header, `fmt ` chunk, and `data` chunk per the Sony DSF 1.01 spec.
- Deinterleaves the file's 4096-byte-per-channel blocks into AOE's byte-granular channel interleave on every `read()`.
- Bit-reverses each byte when the file is stored LSB-first (`bits_per_sample=1`, the usual case) so the wire stream comes out MSB-first.
- Validates the file's declared channels and sampling frequency against the configured `--format` / `--channels`. Mismatches abort at startup — AOEther never resamples.
- Rejects DSD512+ DSF content with a clear error pointing at M8 (packet-splitting extension needed).

The bridge-via-snd-aloop pattern used for PCM (`docs/recipe-roon.md`, `docs/recipe-capture.md`) still does not work for DSD streams because `snd-aloop` is PCM-only. The DSF reader is the intended path for local DSD file playback today.

DFF (`.dff`) files are a tracked follow-up; HQPlayer's NAA remains a reasonable external option for DFF or for content above DSD256 until M8 lands.

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

**Receiver: `snd_pcm_set_params (ch=2 alsa_rate=... fmt=dsd64 alsa=dsd_u8): Invalid argument`** — DAC doesn't advertise DSD_U8 at that rate. See Step 2 above and try `--alsa-format dsd_u32_be` (most common) or `dsd_u16_le`.

**Talker: `AVTP AAF does not carry DSD; use --transport l2 or ip with --format dsd*`** — correct; AAF is PCM-only. Switch transport.

**Receiver: `dropped` counter rises with `rx=0`** — frames are arriving but the format code in the header doesn't match CLI. Make sure both talker and receiver use the same `--format`.

**Receiver: `underruns` counter rises steadily** — DSD bandwidth at DSD256 stereo is ~11 Mbps; if the network or the DAC's USB path can't sustain that, underruns follow. Try a lower DSD rate or check for USB 2.0 hub contention (DSD256+ generally wants a direct USB 2.0 HS connection, no hub in most cases).
