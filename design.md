# Audio-over-Ethernet with Minimal-Processing UAC2 Receivers

**Status:** Draft v0.2 — design doc, pre-implementation
**Audience:** Contributors, reviewers, early adopters
**License:** Apache 2.0 (proposed)

**Changes from v0.1:** Restructured around a three-tier receiver hardware strategy (Raspberry Pi → Marvell ClearFog / NXP i.MX 8M → NXP RT1170 MCU). M1 now targets the RPi, dramatically shortening the path to first audio. The MCU receiver moves to a parallel track starting at M6. Added DSD support (DSD64 through DSD2048) as a first-class format, with bandwidth analysis and a phased rollout.

**Changes from v0.2a:** Corrected the DSD discussion. Linux *host* side (via `snd_usb_audio` quirks) supports native DSD up to **DSD1024**, not capped at DSD256 — the v0.2 wording conflated "stock `f_uac2` gadget capability" with "Linux DSD capability" and was misleading. Gadget-side native DSD is an `f_uac2` patch project, not a fundamental architectural limit, so native DSD is viable on Tier 1/2 with an upstreamable kernel patch rather than being MCU-only. Corrected DoP ceiling to DSD512 (at 1411.2 kHz PCM, where supported) from the earlier DSD256 claim.

## Summary

An open-source system for transporting multichannel PCM and DSD audio over an Ethernet network with deterministic timing, where the receiver forwards packets almost directly into a USB Audio Class 2 (UAC2) endpoint. The talker runs on Linux and does the hard work — clock discipline, packet scheduling, stream management. The receiver is a near-stateless bridge: Ethernet MAC → small jitter buffer → USB IN endpoint, with no sample rate conversion, reformatting, or per-sample math.

The project ships three reference receiver implementations spanning a 100× cost range: a Raspberry Pi 4/5 for cheap bring-up and home use, a Marvell ClearFog or equivalent SBC for hardware-PTP / Milan-class deployments, and an NXP RT1170 MCU for the truly minimal embedded case. All three speak the same wire protocol and present identical UAC2 behavior to the host.

Target audio formats span stereo PCM at 48 kHz up to 22.2-channel PCM at 96 kHz, and DSD64 through DSD2048 (with bandwidth caveats at the highest DSD rates and channel counts).

## Goals

The system must carry bit-exact PCM and DSD audio from a talker on a Linux host to one or more UAC2-presenting receivers, with end-to-end latency under 5 ms for typical configurations and under 2 ms when the network path allows. Receiver-side processing in the audio data path must be limited to header parsing and a small jitter buffer; no sample rate conversion, no reformatting, no per-sample DSP. The wire format must scale from 2 channels at 48 kHz/24-bit up to 32 channels at 96 kHz/24-bit and from DSD64 stereo up to DSD2048 stereo on a single gigabit link, with multi-stream support for higher channel counts. The project must run on commodity hardware and offer a working receiver at three price points (under $100, $150–$300, and $200–$300 for the MCU eval kit).

## Non-goals

Object-based Atmos rendering (metadata-driven) is out of scope; channel-based multichannel covers bed formats up to 22.2 and object rendering belongs in a separate layer on top. Compressed audio formats (AC-4, Dolby TrueHD, DTS, MQA) are out of scope — this is a transparent transport for PCM and DSD only. WiFi transport is out of scope; the timing guarantees require wired Ethernet. Audio effects, mixing, or any DSP is out of scope. Sample rate conversion is out of scope — the receiver never resamples; if the talker and host disagree on rate, that's handled at higher layers or by deploying matched-rate gear.

## Architecture overview

```
┌───────────────────────────┐          ┌──────────────────────────────┐
│  Talker (Linux host)      │          │  Receiver (any tier)         │
│                           │          │                              │
│  ┌─────────┐   ┌────────┐ │          │ ┌────────┐   ┌─────────────┐ │
│  │ Audio   │──▶│ Packet │ │ Ethernet │ │ ENET   │──▶│ Jitter      │ │
│  │ source  │   │ shaper │ │  (TSN at │ │ MAC    │   │ buffer      │ │
│  └─────────┘   └───┬────┘ │  Tier 2+)│ └────────┘   │ (small)     │ │
│                    │      │ ◄──────▶ │              └──────┬──────┘ │
│  ┌─────────┐       ▼      │          │                     ▼        │
│  │ gPTP    │   ┌────────┐ │          │ ┌────────┐   ┌─────────────┐ │
│  │ daemon  │──▶│ NIC    │ │          │ │ gPTP   │   │ USB device  │ │
│  └─────────┘   │ TxTime │ │          │ │ (opt.) │   │ UAC2 IN ep  │ │
│                └────────┘ │          │ └────────┘   └──────┬──────┘ │
└───────────────────────────┘          └─────────────────────┼────────┘
                                                             │ USB
                                                             ▼
                                                    ┌────────────────┐
                                                    │ USB host       │
                                                    │ (DAW, player,  │
                                                    │ DAC, etc.)     │
                                                    └────────────────┘
```

The talker accepts audio from any source (ALSA capture, JACK client, file, synthesized test signal), packages samples into packets with embedded timing information, and emits them on a strict cadence aligned to gPTP (where available). On NICs that support it, the kernel's `etf` qdisc plus `SO_TXTIME` ensures packets hit the wire at precise microsecond-accurate intervals regardless of scheduler jitter. On commodity NICs (e.g., the RPi's onboard Ethernet), software pacing achieves tens-of-µs jitter — adequate for most uses, inadequate for Milan compliance.

The receiver, regardless of tier, has the same three concurrent activities: the audio data path (ideally zero-CPU in steady state), a control path for stream setup and feedback, and a background gPTP task. The audio data path never blocks on the control path.

## Receiver tiers

A single wire protocol supports three receiver implementations targeting different budgets and use cases.

### Tier 1 — Raspberry Pi 4/5 with `f_uac2` gadget

The cheapest and fastest path. Total BOM under $80. Runs stock Raspberry Pi OS (64-bit, Bookworm or later). The Linux kernel's `f_uac2` USB gadget driver handles all UAC2 device-side complexity — descriptors, endpoints, feedback, enumeration — so the receiver application is a small userspace C program that copies samples from an `AF_PACKET` socket to an ALSA playback device that `f_uac2` exposes. No kernel patches, no firmware builds, no MCU toolchain.

Limitations: no hardware PTP timestamping (BCM2711/2712 lacks PTP support on the Ethernet MAC), so PTP runs in software-only mode with ~50–100 µs accuracy. Linux scheduler jitter limits how tight the audio path can get without `PREEMPT_RT`. CBS / TSN shaping is not available on the Pi's NIC. DSD support is limited to DoP encoding (which works through `f_uac2` because DoP is PCM at the wire level); native DSD requires kernel modifications.

### Tier 2 — Marvell ClearFog / NXP i.MX 8M / equivalent Linux SBC

Hardware-PTP-capable Linux SBC, $120–$400. Same userspace receiver code as Tier 1, but with hardware PTP timestamping enabled so `ptp4l` achieves sub-microsecond sync. CBS / TSN shaping available depending on platform. Can target Milan compliance.

Specific candidates with their tradeoffs:

- **Marvell ClearFog Base** (~$140, Armada 388, dual A9): Excellent Ethernet stack, hardware PTP via `mvneta`, TSN-capable triple-PHY switch. USB 2.0 OTG is on a pin header rather than a connector — fine for development, awkward for a finished product.
- **NXP i.MX 8M Mini SOM + carrier** (~$150–$200): Hardware PTP via the ENET_QOS controller, USB OTG on a standard Type-C connector, well-supported by the mainline kernel and Yocto. Probably the cleanest option for a polished reference design.
- **NXP i.MX 8M Plus SOM** (~$250): Same as Mini plus a second Ethernet (TSN-capable), neural processing unit (irrelevant here), more grunt. The right pick if you want to demonstrate two streams with FRER-style redundancy in M8.
- **BeagleBone AI-64** (~$200, TI AM6548): Hardware PTP, USB Type-C device mode, mainline support. Strong community.

### Tier 3 — NXP MIMXRT1170-EVKB (bare-metal MCU)

The flagship "minimal receiver" target, $200 for the eval board, sub-$50 BOM at production scale. Cortex-M7 @ 1 GHz with on-chip Ethernet MAC (with hardware PTP), on-chip USB HS device controller, 2 MB SRAM, full DMA pipeline from ENET RX through to USB TX. No OS in the audio path; the data flow is pure ISR-driven DMA.

This tier is where the "minimal processing" claim shines: zero CPU cycles per sample in steady state. It's also where native DSD (beyond DoP) lives, because we control the entire USB descriptor stack and can expose vendor-specific DSD formats that stock `f_uac2` doesn't support.

### Why three tiers

A single tier would force a bad tradeoff. RPi-only would mean no hardware PTP and no Milan story. MCU-only would scare off contributors who don't want to learn embedded development. Tier 2 alone would miss both the cheap entry point and the embedded showpiece. Three tiers let each constituency get what they need from the same protocol.

## Audio formats

The wire format carries either PCM or DSD payloads, distinguished by a `format` byte in the header. The format byte determines how the receiver interprets `samples_per_frame`, what UAC2 descriptors it advertises, and whether the host sees the device as a PCM source or a DSD source (DoP-encoded PCM, or native DSD on Tier 3).

### PCM

Standard signed integer PCM, little-endian, interleaved by channel. Supported widths: 16-bit, 24-bit packed (3 bytes/sample), 32-bit padded. Supported sample rates: 44.1 kHz, 48 kHz, 88.2 kHz, 96 kHz, 176.4 kHz, 192 kHz, 352.8 kHz, 384 kHz. The receiver advertises matching UAC2 format descriptors at enumeration time.

### DSD (Direct Stream Digital)

DSD is 1-bit oversampled audio at multiples of 64×44.1 kHz. Supported rates:

| Format | Bit rate per channel | Per-microframe (8 kHz) per channel |
|--------|---------------------:|-----------------------------------:|
| DSD64  | 2.8224 Mbps | 44.1 bytes |
| DSD128 | 5.6448 Mbps | 88.2 bytes |
| DSD256 | 11.2896 Mbps | 176.4 bytes |
| DSD512 | 22.5792 Mbps | 352.8 bytes |
| DSD1024 | 45.1584 Mbps | 705.6 bytes |
| DSD2048 | 90.3168 Mbps | 1411.2 bytes |

**Bandwidth implications** for DSD on a single gigabit link (~940 Mbps usable for Class A traffic with CBS at 75% reservation):

| Channels × DSD rate | Wire bandwidth | Fits gigabit? |
|---------------------|---------------:|:--------------|
| 2 × DSD64 | ~6 Mbps | ✓ |
| 2 × DSD2048 | ~190 Mbps | ✓ |
| 6 × DSD2048 (5.1) | ~570 Mbps | ✓ tight |
| 8 × DSD2048 (7.1) | ~760 Mbps | ✓ tight |
| 12 × DSD2048 (7.1.4 Atmos bed) | ~1140 Mbps | ✗ requires 2.5GbE+ |
| 24 × DSD2048 (22.2) | ~2280 Mbps | ✗ requires 10GbE |

DSD2048 multichannel beyond 7.1 requires either a higher-speed link (2.5GbE NICs are now common, 10GbE is widely available on Tier 2 hardware) or stream splitting across multiple parallel gigabit interfaces. The protocol supports both; the deployment guide documents which configurations require which link speeds.

### DSD encoding modes on the wire

Two encodings, selectable per stream:

**DoP (DSD over PCM):** Each 24-bit PCM "sample" carries 16 DSD bits in the lower 16 bits and an alternating marker byte (0x05/0xFA) in the upper 8 bits. PCM rate = DSD rate × 64 ÷ 16 ÷ 44.1k... practically: DoP-DSD64 = 176.4 kHz PCM, DoP-DSD128 = 352.8 kHz PCM, DoP-DSD256 = 705.6 kHz PCM, DoP-DSD512 = 1411.2 kHz PCM. DoP reliably covers **DSD64–DSD256** because 705.6 kHz PCM is widely supported; DoP-DSD512 works end-to-end only where both sides support 1411.2 kHz PCM (a smaller set of hardware). DSD1024 and above exceed any plausible PCM rate and cannot use DoP. The advantage of DoP is universal compatibility — to any USB host or driver, it looks like ordinary PCM, and DACs that recognize the marker bytes switch to DSD playback automatically. The receiver tier doesn't matter for DoP; even Tier 1 with stock `f_uac2` works (subject to the `f_uac2` sample-rate limits for very-high-rate DoP).

**Native DSD:** A UAC2 alternate setting with format type III "Raw Data" carrying the DSD bitstream directly. Required for DSD1024 and above, and preferable to DoP at all rates for its lower overhead and cleaner semantics.

Native DSD support on Linux is asymmetric between host and gadget:

- **Host side (Linux reads from a USB DSD source):** Well-supported in mainline via `snd_usb_audio` quirks in `quirks-table.h`. Recognized DACs (Holo Audio, Topping, Denafrips, Gustard, XMOS-based interfaces, and many more) are handled at up to DSD1024 as of current kernels. Matching happens by VID/PID. This is the code path people use when playing DSD files on Ubuntu — it's solid.
- **Gadget side (Linux presents itself as a USB DSD source):** Stock mainline `f_uac2` does not expose native-DSD alternate settings — configfs only supports PCM format parameters. Adding native DSD to `f_uac2` is an upstreamable kernel patch (estimated ~300–500 lines of kernel code across `f_uac2.c` and a host-side `quirks-table.h` entry matching our project's VID/PID). Viable for Tier 1/2 if we do the kernel work; trivial for Tier 3 MCU because we control the entire USB stack.

macOS and Windows host-side considerations: macOS supports native DSD from a small set of recognized devices and supports DoP universally; Windows native-DSD requires ASIO (ASIO4ALL, Thesycon drivers, or vendor-specific ASIO builds) while DoP works through WASAPI with the right driver. Any gadget using native DSD needs either to match an existing recognized VID/PID or to convince the host-side ecosystem to add a quirk/driver entry for its own.

### Format negotiation

The talker advertises supported formats per stream during stream setup (in M6+ via AVDECC; before that, configured statically). The receiver enumerates UAC2 alternate settings for each format. The USB host selects an alternate setting (i.e., picks a format), and the talker is informed via the control channel; from then on every packet for that stream uses the selected format.

## Wire format

For the initial milestones, a custom EtherType carries a minimal header followed by raw audio payload. M4 switches to AVTP AAF for PCM and adds a vendor-specific AVTP subtype for DSD.

### Ethernet frame layout

```
┌──────────────────┬──────────────────┬──────────┬───────────────────────┐
│ Ethernet header  │ AoE header       │ Audio    │ Padding (if needed    │
│ (14 bytes)       │ (16 bytes)       │ payload  │ for min frame size)   │
└──────────────────┴──────────────────┴──────────┴───────────────────────┘
```

EtherType: `0x88B5` (IEEE Std local experimental EtherType 1) for development; migration to AVTP (`0x22F0`) by M4.

### AoE (Audio-over-Ethernet) header

```
Offset  Field              Size  Type      Notes
0       Magic              1     u8        0xA0 (marker for quick validation)
1       Version            1     u8        0x01
2       Stream ID          2     u16 BE    Identifies the logical stream
4       Sequence number    4     u32 BE    Monotonic, wraps at 2^32
8       Presentation time  4     u32 BE    Low 32 bits of gPTP ns
12      Channel count      1     u8        1..64
13      Format             1     u8        See format codes below
14      Payload count      1     u8        Samples-per-channel for PCM,
                                            DSD-bytes-per-channel for native DSD,
                                            DoP-samples-per-channel for DoP
15      Flags              1     u8        Bit 0: last-in-group; bit 1: discontinuity;
                                            bit 2: marker; bits 3-7 reserved
```

### Format codes

```
0x00  Reserved
0x10  PCM s16le
0x11  PCM s24le-3 (24-bit packed in 3 bytes)
0x12  PCM s24le-4 (24-bit padded in 4 bytes)
0x13  PCM s32le
0x20  DoP-DSD64       (PCM s24le-3 at 176.4 kHz with DoP markers)
0x21  DoP-DSD128
0x22  DoP-DSD256
0x30  Native DSD64    (raw DSD bitstream, 1 bit per sample, packed MSB-first)
0x31  Native DSD128
0x32  Native DSD256
0x33  Native DSD512
0x34  Native DSD1024
0x35  Native DSD2048
```

### Payload sizing

The receiver computes payload length as `channels × bytes_per_unit × payload_count`, where `bytes_per_unit` depends on format (1 for s16le ÷ 2... actually, 2 for s16le, 3 for s24le-3, 4 for s24le-4 and s32le, and 1 for native DSD with `payload_count` interpreted as bytes). The formula is encoded in a small lookup table on the receiver.

### Cadence

One packet per USB microframe at the target rate. For PCM 48 kHz: 8000 packets/sec, 6 samples per packet per channel. For DSD64 native: 8000 packets/sec, 44.1 bytes per packet per channel (alternating 44 and 45 bytes to average 44.1, or send 8000 × 44 + occasional padding — exact scheme TBD in implementation). For DSD2048 native: 8000 packets/sec, 1411.2 bytes per packet per channel — for stereo this is 2822 bytes, exceeding standard MTU. **DSD1024 stereo and DSD2048 mono are the practical single-packet-per-microframe limits at standard MTU.** Above that, multi-packet-per-microframe transmission is used (e.g., DSD2048 stereo = 2 packets per microframe of 1411 bytes payload each), preserving the per-microframe alignment but doubling the packet rate.

### Frame size table

| Config | Payload | Total frame (incl 14B eth + 16B AoE + 4B FCS) |
|--------|--------:|----------------------------------------------:|
| Stereo PCM 48k/24 | 36 B | 70 B (pad to 64-byte minimum) |
| 7.1.4 PCM 48k/24 | 216 B | 250 B |
| 22.2 PCM 48k/24 | 432 B | 466 B |
| Stereo DSD64 native | 88 B | 122 B |
| Stereo DSD256 native | 352 B | 386 B |
| Stereo DSD2048 native | 2822 B | 2856 B (split into 2 packets of 1411 B payload) |
| 5.1 DSD2048 native | 8467 B | split into 6+ packets per microframe |

## Clock architecture

Three clocks exist: the talker's audio source clock, the network time (gPTP), and the USB host's SOF clock. Three modes handle the relationship between them:

### Mode A: Talker is clock master (M1–M3)

The talker emits packets according to its local audio clock. The receiver presents an asynchronous IN UAC2 endpoint; the host accepts whatever the device has each microframe. The host's audio framework absorbs clock drift via resampling or buffering. Correct for most listener applications. This is the M1 default.

### Mode B: gPTP is clock master (M3+, Tier 2+ only)

The talker derives its audio sample clock from gPTP. Multiple talkers on the same network produce phase-aligned output. Receivers remain async-IN on the USB side. Requires hardware PTP timestamping, so this mode unlocks at Tier 2.

### Mode C: USB host is clock master (M7+)

The receiver measures the host's SOF clock rate against gPTP and reports back to the talker. Talker adjusts emission to match. Required when the USB host's DAC is the final clock authority.

### DSD bit clock considerations

DSD doesn't have a "sample" clock in the traditional sense — it has a bit clock at the DSD rate (2.8 MHz for DSD64, 90.3 MHz for DSD2048). For DoP, the underlying PCM sample clock substitutes (176.4 kHz at DSD64, etc.) and behaves like any other PCM clock. For native DSD, the receiver's USB stack still pulls bytes on the microframe cadence (8 kHz), but the byte count per microframe is high and rate matching is done at byte granularity rather than sample granularity. Otherwise the same three modes apply.

## Jitter buffer design

The receiver's jitter buffer absorbs network jitter, clock skew, and USB SOF jitter. Default depth is 4 microframes (500 µs of audio). Implementation differs by tier:

**Tier 1 (RPi):** ALSA's built-in PCM buffer, configured to ~500 µs via `snd_pcm_set_params`. The userspace receiver writes packets into the buffer; ALSA hands them to `f_uac2` on USB SOF. Lock-free in the data path because ALSA handles synchronization internally.

**Tier 2 (Linux SBC):** Same as Tier 1, but with a tighter buffer (250 µs achievable with `PREEMPT_RT` and CPU pinning).

**Tier 3 (MCU):** A hand-written lock-free SPSC ring buffer in MCU SRAM, written by the ENET RX ISR and read by the USB TX ISR. Buffer slots map directly to USB transfer descriptors so the data path is zero-copy.

All tiers report underrun and overrun events to the control plane for monitoring; neither condition blocks the audio path — underruns substitute silence, overruns drop the oldest slot.

## Milestone plan

Two parallel tracks after M5: the Linux receiver track (Tiers 1–2) progressing through M5–M8, and the MCU receiver track (Tier 3) starting at M6 and catching up to feature parity by M8.

### M1 — RPi receiver, stereo PCM, point-to-point, no PTP

**Goal:** Prove the architecture with the smallest possible code path. RPi 4/5 receiver running stock Raspberry Pi OS, talker on any Linux machine, hardcoded stereo at 48 kHz/24-bit, no time sync.

**Deliverables:** Working stereo audio from talker to RPi, presented as a UAC2 device to a USB host. Total receiver code ~150 lines of C plus a configfs setup script.

**Time estimate:** 1 weekend (the simple version), 2 weekends with full test plan and CI.

See the detailed M1 plan below.

### M2 — Multichannel PCM on RPi

**Goal:** Scale M1 to 8, 16, and 32-channel PCM streams without changing the architecture.

**Deliverables:** Talker accepts multichannel ALSA capture or test signals. Receiver's configfs script generates UAC2 descriptors for the advertised channel count. Verified configurations: 2ch, 6ch (5.1), 8ch (7.1), 12ch (7.1.4), 24ch (22.2 — verifies large payloads), all at 48 kHz/24-bit. Documented host-side caveats (Windows multichannel UAC2 quirks).

**Key risks:** Host-side multichannel UAC2 support varies. Linux: solid. macOS: solid up to 16ch, sometimes flaky beyond. Windows: standard UAC2 driver supports up to 8ch reliably; beyond that needs ASIO or third-party drivers.

**Time estimate:** 1–2 weekends.

### M3 — Tier 2 hardware, hardware PTP, higher PCM rates

**Goal:** Bring up the same software stack on Tier 2 hardware with hardware PTP timestamping. Validate sub-µs sync. Add 96 kHz and 192 kHz PCM rates.

**Deliverables:** Receiver firmware/userspace runs on at least one Tier 2 platform (recommend NXP i.MX 8M Mini for cleanest path). Talker uses `SO_TXTIME` for strict packet pacing. Sub-µs PTP sync verified with `pmc`. Two-talker phase alignment measured. RPi tier remains supported with documented "best effort" software-PTP mode.

**Key risks:** Tier 2 hardware bring-up time. Mainline kernel support varies; Yocto / Buildroot setup is non-trivial.

**Time estimate:** 3–4 weekends.

### M4 — AVTP AAF wire format

**Goal:** Switch from the custom AoE header to IEEE 1722 AAF for PCM streams so Milan/AVB listeners can interop. Custom header retained as an option for DSD and for performance testing.

**Deliverables:** Talker emits AVTP AAF for PCM streams. Receiver parses AVTP. Interop test with at least one Milan listener (Hive-controlled Motu AVB or similar). Wire-format selection via configuration flag.

**Time estimate:** 2 weekends.

### M5 — CBS shaping and DoP DSD support

**Goal:** Enable CBS on Tier 2 hardware. Add DoP-DSD support end-to-end (DSD64–DSD256 reliably; DSD512 via DoP where hardware allows). Works on all tiers including stock `f_uac2` because DoP is PCM at the wire level.

**Deliverables:** Linux `cbs` qdisc configured on Tier 2 talker. Audio remains glitch-free under iperf3 background load. DoP-DSD64 / 128 / 256 verified end-to-end with a real DSD-capable DAC on the USB host. DoP-DSD512 attempted and documented with whatever hardware supports 1411.2 kHz PCM.

**Time estimate:** 2 weekends.

### M6 — Minimal AVDECC, plus launch of MCU receiver track

**Goal:** Make talker and Linux receivers discoverable and controllable via AVDECC (Hive, L-Acoustics Network Manager). Simultaneously, begin the Tier 3 MCU receiver track on RT1170 — initial deliverable is M1-equivalent (stereo PCM, no PTP) on bare metal.

**Deliverables:**
- AVDECC: Entity model, ACMP for stream connection, AEM sufficient for Hive enumeration. Built-in web UI as alternative for non-Milan deployments.
- MCU track M1-equivalent: RT1170 firmware with hand-rolled UAC2 stack, ENET RX → USB TX DMA path, stereo at 48 kHz/24-bit. Same wire format as Tier 1/2.

**Key risks:** AVDECC is the project's largest single chunk of code. MCU track has its own learning curve.

**Time estimate:** 6–8 weekends spread across the two tracks (work them in parallel with different contributors if possible).

### M7 — Scale, soak, native DSD

**Goal:** Validate full Atmos at scale on Linux receivers. Ship native DSD on the MCU tier, and in parallel pursue native DSD on Linux tiers via an `f_uac2` patch (upstreamable goal; self-maintained out-of-tree patch if upstream is slow).

**Deliverables:**
- Linux: 7.1.4 PCM Atmos bed and 22.2 streams, 24-hour soak with zero glitches, multi-receiver topologies (one talker, four receivers), documented behavior under cable pulls and packet loss.
- MCU: multichannel PCM on RT1170 (8ch+), native DSD64 through DSD1024 (and DSD2048 for stereo — DSD2048 multichannel requires >gigabit, deferred).
- `f_uac2` native-DSD patch: prototype targeting Tier 1/2 matching a registered project VID/PID, with a companion `quirks-table.h` entry submitted upstream. Goal is native DSD up to DSD1024 on Linux tiers, matching what host-side Linux already achieves in the opposite direction.

**Time estimate:** 4–6 weekends across the tracks, plus separate calendar time for upstream review of the kernel patch.

### M8 — Production-readiness, redundancy, packaging

**Goal:** Move from "working prototype" to "deployable system" across all three tiers.

**Deliverables:** FRER-style stream duplication and seamless failover (Tier 2+). Full AVDECC controller support (Linux talker can act as controller, not just controllable). Documented deployment recipes for home studio, small venue, broadcast facility, audiophile DSD playback. Pre-built distributables: Debian package for Tier 1/2 receiver, NXP firmware image for Tier 3, Debian package for talker. Project website with docs and examples. Conformance test vectors so third parties can validate their own implementations.

**Time estimate:** Ongoing; M8 represents the project's transition rather than a single push.

## M1 detailed plan

### Hardware

- **Receiver:** Raspberry Pi 4 Model B (4 GB) or Raspberry Pi 5 (4 GB or 8 GB). Either works; Pi 5 is faster but Pi 4 has slightly better-documented gadget mode. Total cost ~$60 with the official power supply.
- **USB cable:** USB-C to USB-A (or USB-C to USB-C if your host is USB-C). Pi 4/5 uses its USB-C port for both power and USB device mode; you'll power the Pi externally and use the USB-C only for data.
  - **Important wiring note:** Pi 4 USB-C in device mode draws bus power from the host — verify your host port can supply ~700 mA, or use a USB-C splitter that allows external power injection. Pi 5 has cleaner power management here.
- **Talker:** Any Linux PC. For M1 the NIC doesn't matter (no PTP, no TSN); use whatever is in the machine.
- **Network:** A dumb gigabit switch or a single Cat 6 cable directly between talker and Pi.
- **USB host (audio sink):** Any Linux/macOS/Windows machine. For bring-up, Linux is easiest because ALSA exposes the device cleanly and `arecord` lets you capture and verify.

### RPi setup (one-time)

Edit `/boot/firmware/config.txt`:

```
dtoverlay=dwc2,dr_mode=peripheral
```

Reboot. Then create a setup script `/usr/local/sbin/aoe-gadget-up`:

```sh
#!/bin/sh
modprobe libcomposite

cd /sys/kernel/config/usb_gadget/
mkdir -p aoe && cd aoe

echo 0x1d6b > idVendor          # Linux Foundation
echo 0x0104 > idProduct         # Multifunction Composite Gadget
echo 0x0100 > bcdDevice
echo 0x0200 > bcdUSB

mkdir -p strings/0x409
echo "$(cat /sys/class/net/eth0/address)" > strings/0x409/serialnumber
echo "AudioOverEthernet" > strings/0x409/manufacturer
echo "AoE Receiver"      > strings/0x409/product

mkdir -p functions/uac2.0
# p_ = playback (gadget plays to host, host sees as microphone/input)
echo 0x3   > functions/uac2.0/p_chmask    # stereo (L+R)
echo 48000 > functions/uac2.0/p_srate
echo 3     > functions/uac2.0/p_ssize     # 24-bit samples in 3 bytes

mkdir -p configs/c.1/strings/0x409
echo "AoE Stereo" > configs/c.1/strings/0x409/configuration
ln -s functions/uac2.0 configs/c.1/

# Bind to USB controller
ls /sys/class/udc > UDC
```

Run it on boot via systemd. After this, plugging the Pi's USB-C into a USB host enumerates a stereo UAC2 microphone device.

### Wire format for M1

Hardcoded:
- Stream ID: `0x0001`
- Format: `0x11` (PCM s24le-3)
- Channels: 2
- Sample rate: 48 kHz
- Payload count (samples per frame per channel): 6
- Cadence: 8000 packets/sec, software-scheduled via `timerfd`
- Destination MAC: hardcoded (no discovery in M1)

### Talker (`talker/`)

Single C program, ~250 lines, no dependencies beyond libc and ALSA (for the live-capture audio source).

```
talker/
├── Makefile
├── README.md
└── src/
    ├── talker.c              — main loop, timerfd, raw socket
    ├── audio_source.h        — abstract source interface
    ├── audio_source_test.c   — sine-wave generator
    ├── audio_source_wav.c    — WAV file reader
    ├── audio_source_alsa.c   — ALSA capture (optional, requires libasound2-dev)
    └── packet.c              — AoE header construction
```

Build with `make`. Run with `sudo ./build/talker --iface eno1 --dest-mac aa:bb:cc:dd:ee:ff --source testtone` (raw sockets need `CAP_NET_RAW`).

### Receiver (`receiver/`)

Userspace C program, ~150 lines, depends on libasound2 (ALSA).

```
receiver/
├── Makefile
├── README.md
├── scripts/
│   └── aoe-gadget-up         — configfs setup (above)
└── src/
    ├── receiver.c            — main loop, AF_PACKET socket, ALSA writes
    └── packet.c              — AoE header parsing (shared with talker)
```

Sketch of `receiver.c`:

```c
int sock = socket(AF_PACKET, SOCK_RAW, htons(0x88B5));
// bind to interface, set promisc if needed

snd_pcm_t *pcm;
snd_pcm_open(&pcm, "hw:UAC2Gadget,0", SND_PCM_STREAM_PLAYBACK, 0);
snd_pcm_set_params(pcm, SND_PCM_FORMAT_S24_3LE,
                   SND_PCM_ACCESS_RW_INTERLEAVED,
                   2 /*channels*/, 48000 /*rate*/,
                   1 /*soft_resample=off*/,
                   500 /*latency_us*/);

uint8_t buf[2048];
while (1) {
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    if (n < 14 + 16) continue;
    aoe_hdr_t *hdr = (aoe_hdr_t*)(buf + 14);
    if (hdr->magic != 0xA0 || hdr->version != 0x01) continue;
    uint8_t *payload = buf + 14 + 16;
    snd_pcm_writei(pcm, payload, hdr->payload_count);
}
```

That's essentially the whole receiver. ALSA's PCM buffer is the jitter buffer. `f_uac2` is the USB stack. Linux gives us 95% of the work for free.

### Build and run

```
# On the talker (Linux PC)
cd talker && make
sudo ./build/talker --iface eno1 --dest-mac $(cat ../pi_mac.txt) --source testtone

# On the Pi (one time setup)
sudo /usr/local/sbin/aoe-gadget-up
cd receiver && make
sudo ./build/receiver --iface eth0

# On the USB host (Linux)
arecord -D hw:AudioOverEthernet,0 -f S24_3LE -c 2 -r 48000 -d 10 test.wav
aplay test.wav   # should hear a clean 1 kHz tone
```

### Test plan for M1

**Functional:**
1. 1 kHz test tone plays cleanly. Spectrum analysis: harmonic distortion below -80 dBFS.
2. WAV file loop replays bit-exact (`cmp` after skipping startup transient).
3. Talker stop → host audio goes silent without pop. Talker restart → audio resumes without re-enumeration.
4. USB cable unplug/replug → re-enumerates cleanly.

**Timing:**
5. Measure end-to-end latency by injecting an impulse at the talker's audio source and measuring its arrival at the host. Target: under 2 ms direct-cable.
6. Measure CPU load on the Pi during steady-state playback. Target: under 10% on a single core (most of which is `f_uac2` / kernel work, not our userspace).

**Soak:**
7. 1-hour stereo tone playback with zero underruns reported, zero audible glitches, host-recorded file passes spectrum-analysis check.

### Deliverables checklist for M1

- [ ] Talker compiles cleanly on Ubuntu 22.04 / 24.04 with stock GCC.
- [ ] Receiver compiles cleanly on Raspberry Pi OS Bookworm.
- [ ] `README.md` with hardware BOM, RPi setup steps, build steps, and a "first audio in 30 minutes" quickstart.
- [ ] `docs/design.md` (this document, kept up to date).
- [ ] `docs/wire-format.md` with byte-level spec.
- [ ] `CONTRIBUTING.md` with code style and PR process.
- [ ] GitHub Actions CI: builds talker on Linux x86, builds receiver on Linux ARM (cross-compile in CI, run-tested on real hardware).
- [ ] A short demo video (60–90 s) showing the full pipeline.

### Known deferred items from M1

- No PTP; cadence is `timerfd`-based with millisecond-class scheduling jitter under load. Fine for stereo bring-up, addressed in M3.
- No feedback loop; long-term clock drift between talker and USB host is unhandled (audible after many minutes depending on crystal accuracy).
- No stream setup or discovery; hardcoded everything.
- No error recovery beyond drop-and-log.
- Single stream, single direction (talker → receiver).
- No authentication, no encryption.
- PCM only; DSD support arrives in M5 (DoP) and M7 (native).

All addressed in subsequent milestones.

## Open questions

**Should Tier 1 (RPi) be a permanent supported target or just a bring-up scaffold?** Lean toward permanent — the entry-cost benefit for adoption is real and the maintenance burden is low because it shares 99% of its code with Tier 2.

**Discovery for M6: AVDECC vs. mDNS-SD vs. both?** Both. AVDECC for Milan interop, mDNS-SD for the "home lab just works" path.

**Which PTP profile?** gPTP (IEEE 802.1AS-2020). Plain PTPv2 is more widely deployed but won't interop with Milan.

**MCU: stick with RT1170 or consider alternatives at M6?** RT1170 is the current pick. Alternatives worth a look at M6 kickoff: TI AM6442 (good TSN), STM32H7 (cheaper but less capable USB), NXP S32G (overkill, automotive-grade, expensive).

**Native DSD on Linux gadget side: is `f_uac2` patching worth doing?** Revised opinion: **yes, probably.** The host side of Linux already supports native DSD up to DSD1024 via `snd_usb_audio` quirks, so the asymmetry (host can consume DSD1024 but Linux gadget can only produce DoP) is purely an `f_uac2` gap, not a deep architectural limit. A patch adding Format Type III alternate settings to `f_uac2` is a bounded, upstreamable project. Tier 3 (MCU) remains the simpler path to native DSD, but closing the `f_uac2` gap benefits the whole Linux gadget ecosystem, not just this project.

**Reverse direction (USB → Ethernet, "network microphone")?** Architecturally symmetric with the current design; defer to M8 or later. Same wire format with direction field, same `f_uac2` capability on Linux receivers, same MCU code patterns.

## Appendix A: Reference implementations to study

- **Linux `f_uac2` gadget driver** (`drivers/usb/gadget/function/f_uac2.c` in the kernel tree) — the heart of Tiers 1 and 2. Read it before touching the receiver.
- **Linux `snd_usb_audio` driver** (`sound/usb/`) — host-side reference for async UAC2.
- **dwc2 driver** (`drivers/usb/dwc2/`) — Pi 4/5 USB device controller; helpful for debugging gadget-mode issues.
- **OpenAvnu** (`github.com/Avnu/OpenAvnu`) — AVTP talker/listener, gPTP daemon. Reference but heavyweight.
- **linuxptp** (`linuxptp.sourceforge.net`) — `ptp4l`, `phc2sys`, `pmc`. Used from M3 onward.
- **NXP MCUXpresso SDK** — `usb_device_audio_generator` and `lwip_tcpecho_bm` are starting points for the Tier 3 MCU receiver in M6.
- **Hive** (`github.com/christophe-calmejane/Hive`) — open-source AVDECC controller, validates M6 work.
- **DoP specification** (`dsd-guide.com/sites/default/files/white-papers/DoP_openStandard_1v1.pdf`) — authoritative reference for the marker-byte encoding.
- **dCS / Mytek / Holo Audio public docs** — reference for native-DSD UAC2 alternate-setting conventions.

## Appendix B: Key design decisions and rationale

**Why three hardware tiers?** Single-tier projects force bad tradeoffs. RPi-only would cap the project's pro AV ambitions. MCU-only would scare off most contributors. Tier 2 alone would miss both the cheap entry point and the embedded showpiece. The wire protocol is the same across all three; only the implementation differs, so the cost of supporting three tiers is mostly upfront design discipline.

**Why RPi as M1 target instead of MCU?** Linux's `f_uac2` driver gives us the entire UAC2 device-side stack for free. M1 on RPi is ~150 lines of userspace C; M1 on MCU is several thousand lines of firmware. For a project that needs to demonstrate viability quickly and recruit contributors, the RPi-first ordering is dramatically better.

**Why a custom wire format for M1 instead of AVTP?** AVTP parsing is several hundred lines on a constrained receiver. For M1 the goal is to prove the architecture with the smallest possible code. Migrating to AVTP in M4 is straightforward because the per-microframe cadence and presentation-time concept are unchanged.

**Why DoP first and native DSD later?** DoP works on stock Linux for both Tier 1 (gadget side, via stock `f_uac2`) and any host OS — at the wire level it's PCM. Adding DoP support is a wire-format extension, not a USB-stack change. Native DSD on Tier 1/2 Linux gadgets requires patches to `f_uac2` to add Format Type III alternate settings (roughly a few hundred lines, upstreamable but not free); native DSD on Tier 3 MCU is straightforward because we own the USB descriptor stack. So DoP ships first because it works everywhere with no kernel changes; native DSD ships next on the MCU tier (M7) and then on Linux tiers (M7 stretch / M8) as the patch matures.

**Why support DSD2048 at all?** It's at the very edge of audiophile relevance and almost no commercial gear handles it today. But the bandwidth math fits gigabit comfortably for stereo, and supporting it costs little once the wire format is in place — it's just a different format code and a larger payload. Demonstrating DSD2048 working over a $50 MCU-to-USB bridge is the kind of "wait, you can do *that*?" moment that grows a project's audience.

**Why Apache 2.0 not GPL?** Pro AV vendors are nearly all commercial; GPL would block their integration and contribution. The patent grant in Apache 2.0 also protects contributors from TSN-related patent claims better than MIT.

**Why async-IN UAC2 on the receiver?** Async puts clock authority on the talker / network side, matching the pro AV model where the network is the clock master. Sync IN would force the receiver to clock-match USB SOF, requiring SRC (violates minimal-processing) or PLL (complex). Async IN is universally supported by host audio frameworks.

**Why one packet per microframe (mostly)?** Lower latency. The exception is high-rate DSD multichannel where a single packet exceeds MTU; there we use multiple packets per microframe with a `last-in-group` flag, preserving microframe alignment.

## Appendix C: Risk register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| `f_uac2` kernel quirks (older Pi OS kernels have rough edges around large channel counts and high sample rates) | Medium | Medium | Pin to a known-good kernel version; document required kernel for each milestone; report and patch upstream where needed |
| Pi 4 USB-C device-mode power negotiation flakiness | Medium | Low | Document the externally-powered-Pi setup; recommend Pi 5 if available |
| Tier 2 hardware bring-up time blows past M3 estimate | High | Medium | Pick one Tier 2 platform (i.MX 8M Mini recommended) and ignore the others until M3 ships |
| Milan spec edge cases not surfaced until M4 | Medium | High | Start reading Milan spec during M2; engage with Avnu Alliance early |
| Windows multichannel UAC2 support poor beyond 8ch | High | Medium | Document ASIO requirement; declare 8ch the supported Windows ceiling for M2; revisit at M7 |
| Native-DSD descriptor incompatibility with host drivers (especially Windows, macOS) | Medium | Medium | Validate against multiple host OSes before committing format codes; reuse existing `snd_usb_audio` quirks conventions on Linux (proven at DSD1024); plan for ASIO-based Windows workflow; accept that macOS native-DSD support is narrower and DoP may remain the default there |
| `f_uac2` upstream patch rejected or slow to merge | Medium | Low | Maintain as out-of-tree patch for Tier 1/2; Tier 3 (MCU) is unaffected; DoP remains the fallback on any unpatched kernel |
| RT1170 USB HS bandwidth insufficient for DSD2048 multichannel | Medium | Low | DSD2048 multichannel is already gigabit-bound; the MCU constraint matches; document supported DSD/channel combinations per tier |
| Contributors don't show up | Medium | Medium | Build in public; ship a working demo on cheap hardware before announcing |
