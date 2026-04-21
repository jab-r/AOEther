# Audio-over-Ethernet for USB DACs

**Status:** Draft v1.3 — design doc, M1 implementation in progress
**Audience:** Contributors, reviewers, early adopters
**License:** Apache 2.0 (proposed)

**Major revision from v0.3:** Architectural pivot to **Topology B (player mode)** as the primary receiver architecture. The receiver is the USB *host* driving a USB DAC, not the USB *gadget* presented to an upstream computer. This sidesteps the `f_uac2` patching problem entirely because Linux's `snd_usb_audio` already supports native DSD up to DSD1024 via per-DAC quirks. Topology A (gadget mode, for pro-AV / DAW integration) is deferred to a later-milestone "pro" track.

**Changes from v1.0:** Added **transport-mode abstraction**. The AoE header is unchanged, but the outer wrapper can be raw Ethernet (L2, default for deterministic wired deployments), AVTP (for Milan interop), or IP/UDP (for WiFi, routed networks, and AES67 interop). This acknowledges reality: WiFi is increasingly used for audio, Ravenna and AES67 are IP-based, and a single-transport design forecloses too much of the addressable user base. Milestone plan reshuffled accordingly: M4 is now "Alternative transports," pushing subsequent milestones down by one.

**Changes from v1.1:** **Ravenna / AES67 interop promoted to its own milestone (M9).** In v1.1 it was a stretch goal inside M8, which was already overloaded with scale/soak/DSD1024-2048/packaging. AES67 interop is a substantial piece of work (RTP encapsulation, SDP session description, SAP announcement, PTPv2 sync) and the Ravenna/AES67 ecosystem is large enough to deserve proper scope. M8 now focuses on scale/soak/DSD-high-rates/packaging without the AES67 distraction.

**Changes from v1.2:** **Mode C (DAC-disciplined clock) promoted from "M8+" to M1 baseline**, reframed as extending UAC2 asynchronous feedback across Ethernet. Mode A (talker-clock open-loop) is reclassified as diagnostic-only — it does not survive real-world crystal drift for more than tens of seconds at typical oscillator tolerance, and shipping M1 without Mode C would contradict M1's own 1-hour soak test. The wire format gains a small control-frame subprotocol on EtherType `0x88B6` carrying Q16.16 samples-per-ms feedback, modeled directly on UAC2 HS feedback-endpoint format. M1's time estimate rises from 1 weekend to 2 weekends. M8's clock-related scope shrinks accordingly.

## Summary

An open-source system for transporting multichannel PCM and DSD audio over Ethernet into a USB Digital-to-Analog Converter, with the receiver acting as a USB host driving the DAC. The talker (Linux) handles clock discipline, packet scheduling, and stream management. The receiver is a small program that copies samples from Ethernet to the DAC via the host OS's UAC2 driver, with no sample rate conversion or per-sample DSP.

The project ships three reference receiver implementations at different price points: a Raspberry Pi 4/5 for $60 bring-up and hi-fi listening, a Marvell ClearFog or NXP i.MX 8M SBC for hardware-PTP deployments, and an NXP RT1170 MCU as a bare-metal network-streamer endpoint. All three speak the same wire protocol, which carries equally well over raw Ethernet (for deterministic wired deployments, default), AVTP (for Milan interop), or IP/UDP (for WiFi and routed networks, with AES67 interop as a subset).

Target audio formats span PCM (stereo at 48 kHz up to 32 channels at 96 kHz) and DSD (DSD64 through DSD2048, subject to bandwidth limits). **Native DSD up to DSD1024 works on Linux tiers without any kernel patches** because `snd_usb_audio` already supports it; DSD2048 requires MCU-tier work or an ALSA extension.

The initial use cases are audiophile listening-room streaming (a DAC in the rack, a talker elsewhere on the network) and small multichannel setups (5.1 / 7.1.4 via a multichannel USB DAC). WiFi deployments are supported from M4 onward via the IP/UDP transport mode, with documented timing-accuracy tradeoffs. Pro-AV integration via Milan interop and DAW integration via UAC2-gadget mode are both on the roadmap but deliberately deferred until the core is solid.

## Goals

The system must carry bit-exact PCM and DSD audio from a talker on a Linux host to one or more receivers that drive USB DACs, with end-to-end latency under 5 ms typical and under 2 ms when network conditions allow. Receiver-side processing in the audio data path is limited to header parsing and a small jitter buffer; no sample rate conversion, no reformatting, no per-sample DSP. The receiver must discover its attached DAC's capabilities (rates, formats, channel counts, DSD support) and advertise those to the talker so streams get formatted correctly. The wire format must scale from 2-channel PCM at 48 kHz up to 32-channel PCM at 96 kHz, and from DSD64 stereo up to DSD2048 stereo on a single gigabit link. The project must offer a working receiver at three price points (under $100, $150–$300, $200–$300 for the MCU eval kit) using the same protocol.

## Non-goals

Object-based Atmos rendering is out of scope; channel-based multichannel covers bed formats up to 22.2 and object rendering belongs in a separate layer. Compressed formats (AC-4, Dolby TrueHD, DTS, MQA) are out of scope. Effects, mixing, or any DSP is out of scope. Sample-rate conversion is out of scope — the receiver never resamples. **Milan-grade timing over WiFi is out of scope** — consumer WiFi lacks hardware PTP and TSN; the IP/UDP transport mode works over WiFi with documented best-effort timing for single-receiver use cases, not for multi-receiver phase-aligned deployments. **Topology A (gadget mode / UAC2 device emulation) is deferred**, not rejected — it's a compelling feature for pro-AV and DAW integration that belongs on a later milestone once the core is stable.

## Architecture overview

```
┌───────────────────────────┐          ┌──────────────────────────────┐
│  Talker (Linux host)      │          │  Receiver (any tier)         │
│                           │          │                              │
│  ┌─────────┐   ┌────────┐ │          │ ┌────────┐   ┌─────────────┐ │
│  │ Audio   │──▶│ Packet │ │ Ethernet │ │ ENET   │──▶│ Jitter      │ │
│  │ source  │   │ shaper │ │ (TSN at  │ │ RX     │   │ buffer      │ │
│  └─────────┘   └───┬────┘ │ Tier 2+) │ └────────┘   └──────┬──────┘ │
│                    │      │ ◄──────▶ │                     │        │
│  ┌─────────┐       ▼      │          │ ┌────────┐   ┌──────▼──────┐ │
│  │ gPTP    │   ┌────────┐ │          │ │ gPTP   │   │ UAC2 host   │ │
│  │ daemon  │──▶│ NIC    │ │          │ │ (opt.) │   │ (ALSA or    │ │
│  └─────────┘   │ TxTime │ │          │ └────────┘   │ MCU stack)  │ │
│                └────────┘ │          │              └──────┬──────┘ │
└───────────────────────────┘          └─────────────────────┼────────┘
                                                             │ USB
                                                             ▼
                                                    ┌────────────────┐
                                                    │ USB DAC        │
                                                    │ (Holo May,     │
                                                    │ Topping D90,   │
                                                    │ RME ADI-2, etc)│
                                                    └────────────────┘
```

The talker accepts audio from any source (ALSA capture, JACK client, file, synthesized test signal), packages samples into packets per the agreed stream format, and emits them on a cadence aligned to USB microframes. The receiver enumerates its attached DAC, discovers what the DAC supports, advertises those capabilities to the talker via the control plane, and then forwards incoming samples to the DAC through the host OS's UAC2 driver. No resampling, no format conversion — if the talker and DAC disagree on format, the stream setup fails cleanly and the talker either adapts or the user picks a different stream configuration.

## Receiver tiers

A single wire protocol supports three receiver implementations, all using Topology B (receiver as USB host).

### Tier 1 — Raspberry Pi 4/5 + any USB DAC

The cheapest path, total cost under $80 for the Pi plus whatever DAC you already own. Raspberry Pi OS (64-bit, Bookworm or later) includes `snd_usb_audio` with full native DSD support via `quirks-table.h` entries. The receiver application is a small userspace C program that reads from an `AF_PACKET` socket and writes to the ALSA PCM device the kernel exposes for the DAC. Stock kernel, no patches, no gadget-mode gymnastics.

DSD reality check: if your DAC is in `quirks-table.h` (most serious DSD-capable DACs are — Holo Audio May/Spring, Topping D70/D90/E70, Denafrips Ares/Pontus/Terminator, Gustard X series, RME ADI-2, many XMOS-based designs), native DSD works today at whatever rate the DAC claims. The user in this project reports DSD1024 working on Ubuntu via this exact path.

Limitations: no hardware PTP (BCM2711/2712 lacks PTP support), so time sync is software-only (~50–100 µs accuracy). No TSN / CBS on the Pi's NIC. These are addressed by moving to Tier 2 for deployments that need them.

### Tier 2 — Marvell ClearFog / NXP i.MX 8M / BeagleBone AI-64

Linux SBC with hardware PTP timestamping and TSN-capable Ethernet, $120–$400. Same userspace code as Tier 1. Hardware PTP unlocks sub-microsecond `ptp4l` sync; CBS / TSN shaping becomes available. This is the tier that supports Milan-interop ambitions.

Candidates:

- **NXP i.MX 8M Mini SOM + carrier** (~$150–$200): hardware PTP via ENET_QOS, USB-A host ports for DACs, mainline kernel support. Probably the cleanest choice.
- **NXP i.MX 8M Plus SOM** (~$250): same plus a second TSN-capable 1 GbE port (useful for redundancy in M8).
- **Marvell ClearFog Base** (~$140): excellent Ethernet stack with hardware PTP via `mvneta`, TSN-capable. USB-A host ports available; no USB device-mode issues to worry about because we don't use it in Topology B.
- **BeagleBone AI-64** (~$200, TI AM6548): hardware PTP, mainline support, community-friendly.

### Tier 3 — NXP MIMXRT1170-EVKB as bare-metal USB host

The flagship "tiny network streamer" target, $200 for the eval board and sub-$50 BOM at volume. Cortex-M7 @ 1 GHz, on-chip ENET MAC with hardware PTP, on-chip USB HS **host** controller, 2 MB SRAM. Bare-metal firmware implements:

- ENET RX / TX with lwIP or a hand-rolled stack
- A small UAC2 host stack that enumerates the attached DAC, parses descriptors, and streams to it via the DAC's USB isochronous OUT endpoint
- gPTP as a low-priority background task
- Per-DAC quirks (VID/PID-keyed format tables) for DSD-capable devices

This tier is where "sub-$50 BOM streamer that does DSD2048 natively" becomes real. Several commercial audiophile streamers (Sonore microRendu, Allo USBridge, SOtM sMS-200) use exactly this architecture — an SoC with Ethernet in and USB-A out — except closed-source and usually more expensive.

### Why three tiers, all Topology B

The RPi tier covers entry-level hi-fi and development. The Tier 2 SBC tier covers Milan-class deployments where hardware PTP matters. The MCU tier covers product-grade embedded streamers. All speak the same wire protocol; all use the same capability-discovery model; the only real difference is how each one talks to its DAC (ALSA on Tier 1/2, hand-rolled USB host stack on Tier 3).

## Capability discovery

A distinguishing feature of this architecture: **the receiver's DAC is the ground truth for format selection**. The receiver enumerates its attached DAC, extracts capabilities, advertises them to the talker, and the talker picks a compatible stream format.

### What the receiver queries

On Tier 1/2 (Linux), the kernel has already enumerated the DAC by the time our userspace program starts. We read capabilities via:

- `/proc/asound/cardN/stream0` — lists supported formats (PCM rates, PCM formats, DSD formats) per USB interface alternate setting
- ALSA's `snd_pcm_hw_params_*` APIs — probes supported rates, channel counts, format codes
- `/sys/bus/usb/devices/...` — VID/PID, which we cross-reference against our own DAC profile database for additional metadata (DoP vs native-DSD preference, preferred buffer sizes, known quirks)

On Tier 3 (MCU), the USB host stack enumerates the DAC directly: reads device descriptor (VID/PID), configuration descriptors, UAC2-specific descriptors (format type, sample rates, channel map), and locates isochronous OUT endpoints. For DSD-capable DACs that don't follow stock UAC2 format-type conventions, a per-VID/PID quirks table adapts.

### What the receiver advertises

After discovery, the receiver advertises its capabilities via the control plane. In M6+ this is AVDECC (entity descriptor lists supported stream formats matching DAC capability). Before M6 it's a simpler mechanism: mDNS-SD TXT records, or a small JSON document over a known TCP port, or hardcoded at the talker for M1 bring-up.

Capability record (conceptual):

```
{
  "receiver_id": "aoe-rcvr-<uuid>",
  "dac": {
    "vid": "0x262a", "pid": "0x1048",
    "name": "Holo Audio May",
    "quirk": "holo_audio"
  },
  "streams": [
    { "format": "PCM_s24le_3", "channels": 2, "rates": [44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000, 705600, 768000] },
    { "format": "DSD_native",  "channels": 2, "rates": [64, 128, 256, 512, 1024] },
    { "format": "DSD_DoP",     "channels": 2, "rates": [64, 128, 256] }
  ]
}
```

The talker inspects the list, picks a format matching the user's intended stream config, and starts sending. If the user requests a format the DAC doesn't support, the talker rejects stream setup cleanly and reports the mismatch.

### Negotiation flow

1. Receiver boots, enumerates DAC, builds capability record.
2. Receiver advertises via mDNS-SD (before M6) or AVDECC (M6+).
3. Talker discovers receivers on the network.
4. User (or automation) requests a stream from talker → receiver with a specific format.
5. Talker checks format is in receiver's capability record. If yes, starts sending. If no, rejects with a diagnostic.
6. Receiver configures its ALSA / MCU USB output for the agreed format and begins consuming packets.
7. Stream teardown is explicit (via control plane) or implicit (heartbeat timeout).

## Audio formats

### PCM

Standard signed-integer PCM, little-endian, interleaved. Widths: 16-bit, 24-bit packed, 32-bit. Rates: 44.1 kHz through 768 kHz (as advertised by the DAC). Channel counts: 1 through 64, subject to DAC support and stream bandwidth.

### DSD

1-bit oversampled audio at 64×44.1 kHz multiples. On Tier 1/2, ALSA's native DSD formats (`SND_PCM_FORMAT_DSD_U8`, `SND_PCM_FORMAT_DSD_U16_LE`, `SND_PCM_FORMAT_DSD_U32_LE`, `SND_PCM_FORMAT_DSD_U32_BE`) carry DSD bytes directly to the DAC via `snd_usb_audio`'s per-DAC quirks. On Tier 3, the UAC2 host stack speaks the DAC's vendor-specific native-DSD alternate setting.

Supported rates: DSD64, DSD128, DSD256, DSD512, DSD1024, DSD2048. Specific DACs will support a subset; capability discovery determines which.

Bandwidth summary on a single gigabit link (~940 Mbps usable for Class A reservation):

| Configuration | Wire bandwidth | Fits gigabit? |
|---------------|---------------:|:--------------|
| Stereo DSD64 | ~6 Mbps | ✓ |
| Stereo DSD2048 | ~190 Mbps | ✓ |
| 5.1 DSD2048 | ~570 Mbps | ✓ tight |
| 7.1 DSD2048 | ~760 Mbps | ✓ tight |
| 7.1.4 DSD2048 | ~1140 Mbps | ✗ requires 2.5 GbE |
| 22.2 DSD2048 | ~2280 Mbps | ✗ requires 10 GbE |

Multichannel DSD beyond 7.1 at DSD2048 requires faster links (2.5 GbE consumer NICs are common, 10 GbE available on Tier 2 hardware) or stream splitting across parallel interfaces.

### DoP (compatibility mode)

DSD over PCM encodes DSD bits into PCM frames with marker bytes. Useful as a compatibility fallback on hosts or DACs that prefer DoP over native DSD (some macOS configurations, Windows without ASIO, older DACs). DoP carries DSD64–DSD256 universally and DSD512 where 1411.2 kHz PCM is supported. DSD1024+ cannot use DoP.

DoP is a **format code in the wire protocol**, not a primary path. The receiver advertises DoP capability when the DAC recognizes DoP markers (most DSD-capable DACs do). Streams default to native DSD when both sides support it; DoP is selected only when needed for compatibility.

### Why native DSD is primary (not DoP)

The earlier versions of this doc had DoP as the primary DSD path because it works on stock `f_uac2` (Topology A). Topology B sidesteps `f_uac2` entirely, and `snd_usb_audio` (host side) has had native DSD since forever. DoP has a 33% bandwidth overhead and a DSD512 rate ceiling; native DSD has neither. The audiophile audience that drives DSD usage uses native DSD. So native is the intended mode; DoP is the legacy fallback.

## Wire format

Custom EtherType for M1–M3; AVTP AAF for PCM (plus vendor AVTP subtype for DSD) from M4.

### Ethernet frame layout

```
┌──────────────────┬──────────────────┬──────────┬───────────────────────┐
│ Ethernet header  │ AoE header       │ Audio    │ Padding (if needed    │
│ (14 bytes)       │ (16 bytes)       │ payload  │ for min frame size)   │
└──────────────────┴──────────────────┴──────────┴───────────────────────┘
```

EtherType `0x88B5` for development, migration path to AVTP (`0x22F0`) in M4.

### AoE header

```
Offset  Field              Size  Type      Notes
0       Magic              1     u8        0xA0
1       Version            1     u8        0x01
2       Stream ID          2     u16 BE
4       Sequence number    4     u32 BE
8       Presentation time  4     u32 BE    Low 32 bits of gPTP ns
12      Channel count      1     u8        1..64
13      Format             1     u8        See format codes
14      Payload count      1     u8        Interpretation depends on format
15      Flags              1     u8        Bit 0: last-in-group; bit 1: discontinuity;
                                            bit 2: marker; bits 3-7 reserved
```

### Format codes

```
0x00  Reserved
0x10  PCM s16le
0x11  PCM s24le-3
0x12  PCM s24le-4
0x13  PCM s32le
0x20  DoP-DSD64
0x21  DoP-DSD128
0x22  DoP-DSD256
0x23  DoP-DSD512     (requires 1411.2 kHz PCM capability)
0x30  Native DSD64
0x31  Native DSD128
0x32  Native DSD256
0x33  Native DSD512
0x34  Native DSD1024
0x35  Native DSD2048
```

For native DSD, `payload_count` is interpreted as DSD bytes per channel in this packet (8 DSD bits per byte, MSB-first). For PCM and DoP, it's samples per channel.

### Cadence

One packet per USB microframe at the target rate. For PCM 48 kHz: 8000 pps, 6 samples per channel per packet. For native DSD2048 stereo: 2822 bytes per microframe per channel; exceeds MTU, split into 2 packets per microframe with `last-in-group` flag on the second. Microframe alignment is preserved across splits.

## Transport modes

The 16-byte AoE header and its audio payload are identical across all transport modes. Only the outer wrapper changes. This keeps the receiver's parsing logic trivial and lets us meet different deployments on their own terms.

### Mode 1 — Raw Ethernet (L2), EtherType `0x88B5`

The default for M1–M3. AoE frame rides directly in an Ethernet II frame with a custom EtherType. No IP layer, no UDP overhead. Requires talker and receiver to be on the same broadcast domain (same switch or directly connected). Benefits: lowest overhead, easiest path to deterministic timing with `SO_TXTIME` + `etf` qdisc, aligns with how TSN shaping is applied. Drawbacks: does not traverse routers, behaves poorly across WiFi bridges, foreign to IP-centric network operators.

### Mode 2 — AVTP (L2), EtherType `0x22F0` (M5)

IEEE 1722 AVTP encapsulation for Milan interop. PCM streams use AAF (AVTP Audio Format). DSD streams use a vendor-defined AVTP subtype (Milan does not standardize DSD; this is our own). Same broadcast-domain constraint as Mode 1. Unlocks interop with the Milan ecosystem for PCM.

### Mode 3 — IP/UDP (M4)

AoE header and payload carried as the body of a UDP datagram over IPv4 or IPv6. Dedicated UDP port (TBD — will request IANA assignment; interim use port 8805). Talker sends unicast (to a receiver's IP) or multicast (to a 239.x.x.x IPv4 address or ff0e::/16 IPv6 address). Adds 28 bytes of IPv4+UDP overhead, 48 bytes for IPv6+UDP. Works across routers, across VLANs, and over WiFi. This is the mode that unlocks the "audiophile Roon-style listening room where the network is WiFi" use case.

QoS: talker tags packets with DSCP `EF` (Expedited Forwarding) so WiFi APs route them to WMM AC_VO (Voice access category) and wired switches prioritize them. This is advisory; no formal reservation.

Timing: gPTP does not work over WiFi in any practical sense today, so Mode 3 over WiFi uses **software PTP** (sync via PTPv2 over UDP, 100s of µs accuracy) or **no sync** (receiver buffers aggressively and relies on async USB feedback from the DAC). Mode 3 over wired gigabit with hardware PTP is viable and gets sub-µs sync; it's the WiFi leg specifically that's timing-degraded.

### Mode 4 — RTP / AES67 (M9)

For PCM interop with the AES67 / Ravenna ecosystem. PCM samples are carried in RTP-over-UDP following the AES67 profile: L16 / L24 payload types, 1 ms packet time (or 125 µs for low-latency), 48 kHz baseline. Session described by SDP, discovered via SAP multicast. PTP required on both sides for any meaningful interop.

Mode 4 is a strict subset of Mode 3 for PCM and does not carry DSD (AES67 doesn't). It exists for ecosystem reach — any AES67 receiver (Merging Anubis, Neumann MT 48, Ravenna gear, Dante-with-AES67-enabled devices, `aes67-linux-daemon` on Linux) can consume our streams, and we can consume theirs.

### Transport × deployment matrix

| Mode | Wired LAN | Wired routed | WiFi | Milan interop | AES67 interop | Carries DSD | Typical timing |
|------|:---------:|:------------:|:----:|:-------------:|:-------------:|:-----------:|---------------|
| 1: L2 EtherType | ✓ | ✗ | ✗ | ✗ | ✗ | ✓ | Hardware PTP (Tier 2+) |
| 2: AVTP L2 | ✓ | ✗ | ✗ | ✓ (PCM) | ✗ | ✓ | Hardware PTP (Tier 2+) |
| 3: IP/UDP | ✓ | ✓ | ✓ | ✗ | ✗ | ✓ | HW PTP wired / SW PTP on WiFi |
| 4: RTP/AES67 | ✓ | ✓ | ✓ | ✗ | ✓ | ✗ (PCM only) | HW PTP wired / degraded on WiFi |

### Mode selection

A stream is in exactly one mode end-to-end. The talker emits in the mode configured for that stream; the receiver accepts on the corresponding listener. Receivers can listen on multiple modes simultaneously (different streams, different modes). The control plane announces which modes a stream uses; AVDECC (M7) handles this for Milan-compatible modes, mDNS-SD / SAP handle it for IP modes.

## Clock architecture

Three independent physical clocks appear in the system: the talker's audio source clock, the network's gPTP time (when PTP is available), and the DAC's internal clock. The DAC's clock is the only one that matters for audio correctness at the output — it's the clock that samples hit the physical world at. Every other clock either tracks it or causes audible artifacts.

### AoE extends UAC2 async feedback across Ethernet

The `snd_usb_audio` kernel driver on the receiver already slaves its isochronous USB OUT rate to the DAC via UAC2 **asynchronous feedback**: the DAC's feedback endpoint reports a Q16.16 samples-per-1ms value, and the host adjusts how many samples it places in each isoc microframe to match. The core AOEther clock insight is that **we extend this same feedback loop one hop upstream, across Ethernet.**

- The receiver observes its ALSA consumption rate (already locked to the DAC by `snd_usb_audio`'s own async-feedback handling), re-expresses it as a Q16.16 samples-per-1ms value, and sends it back to the talker in a small **control frame** (EtherType `0x88B6`; see [`wire-format.md`](wire-format.md) §"Control frames").
- The talker maintains a fractional sample accumulator driven by the latest feedback value. Each outgoing data frame carries a *variable* number of samples — the integer part of the accumulator; the fractional residual carries to the next packet. This is exactly how a USB host adjusts microframe payload size under UAC2 async.
- Packet cadence (the `timerfd` tick on the talker) stays at exactly nominal microframe rate. **Payload count, not timer period, absorbs the drift.** No ppm-precision timer math is needed on the talker.

Semantically the talker is a USB host with a very long cable; the receiver is the DAC's async feedback endpoint extended across the network.

### Operating modes

**Mode A — open-loop (diagnostic only).** Talker emits at exactly nominal rate with a fixed sample count per packet and ignores any feedback. Receiver consumes whatever arrives. Fill level drifts monotonically with the crystal offset between talker and DAC (~1 sample/sec at 20 ppm) and exhausts the jitter buffer within seconds to minutes. Mode A is useful for bring-up and wire-format testing; it is **not a viable operating mode for listening.** Earlier versions of this doc called Mode A "sufficient for casual use" — that was incorrect and is retracted here.

**Mode C — UAC2-extended feedback (baseline, from M1).** Receiver emits FEEDBACK control frames at 20–50 Hz (see wire-format.md). Talker maintains a fractional sample accumulator driven by the latest feedback value. Payload size varies per packet; packet cadence is constant. Single-receiver, single-stream control loop; stability is trivial because the receiver side has a large smoothing window (ALSA buffer fill level over tens of ms) and the talker side applies ppm-scale corrections. Mode C works over any transport that can carry its feedback frames, including IP/UDP over WiFi (at higher jitter-buffer cost).

**Mode B — gPTP-disciplined emission (M3+, Tier 2+).** On hardware-PTP-capable platforms the talker can additionally discipline its *emission* clock to gPTP, giving multi-receiver phase alignment. This does not replace Mode C — consumer USB DACs are free-running crystals that nothing in the network disciplines, so every receiver still needs Mode C to rate-match locally. Mode B adds a shared time reference so multiple receivers agree on *when* to present a sample; Mode C handles *how fast* samples should flow into each DAC. The two modes do different jobs and coexist.

In a Milan-grade pro-AV deployment where endpoints have gPTP-disciplined media clocks (rather than free-running crystals), Mode B alone is sufficient because all clocks in the system track gPTP. AOEther supports that deployment but doesn't require it.

### WiFi timing caveat

Mode C over WiFi works — the feedback frame is just a UDP datagram in Mode 3, and WiFi jitter affects required buffer depth (10–50 ms typical) not clock discipline. Mode B over WiFi does not work: gPTP needs hardware timestamping at both ends of every WiFi hop, which consumer WiFi hardware does not provide. Single-receiver Mode C over WiFi is fine; multi-receiver phase alignment over WiFi is not.

## Jitter buffer

The receiver's jitter buffer sits between the ENET RX path and the USB OUT path. Default 500 µs depth (4 USB microframes).

- **Tier 1/2:** ALSA's PCM buffer. Write via `snd_pcm_writei`; ALSA + `snd_usb_audio` hand samples to the DAC on each USB SOF. Size tuned via `snd_pcm_set_params` latency argument.
- **Tier 3:** Hand-written lock-free SPSC ring in MCU SRAM, slots mapped to USB transfer descriptors for zero-copy.

Underruns substitute silence (rather than blocking). Overruns drop oldest slot. Both report to the control plane for monitoring but never block the data path.

## Milestone plan

### M1 — RPi + USB DAC, stereo PCM, Mode C feedback, real music sources, no PTP

**Goal:** Ship an AOEther that's actually usable for listening, not just a test-tone demo. Plug a USB DAC into a Raspberry Pi, stream stereo PCM from a Linux talker whose input is real music — from Roon, a UPnP controller, AirPlay, Spotify, or any application playing through the OS audio stack — and hear clean audio indefinitely. Includes the Mode C feedback loop from day one — an M1 without it fails its own 1-hour soak test (see §"Clock architecture" for why).

**Approach to music sources:** the talker gains an ALSA capture source. Everything else — Roon (via RoonBridge), UPnP (via gmrender-resurrect or rygel), AirPlay (via shairport-sync), Spotify (via librespot), OS audio (via PipeWire/Pulse) — integrates through the kernel's `snd-aloop` module: the external daemon plays to `hw:Loopback,0,N`, the talker captures from `hw:Loopback,1,N`. No per-ecosystem code in AOEther; one recipe per ecosystem in `docs/recipe-*.md`.

This also means the DAC clock propagates all the way up to the source daemon for free — see §"Clock architecture". Adaptive daemons (PipeWire, shairport-sync, RoonBridge) adapt glitch-free; naïve daemons (gmrender, librespot) may xrun rarely and self-recover.

**Deliverables:** Working stereo audio with Mode C clock discipline, real music playback via at least one of {Roon, UPnP, system capture}, talker + receiver in one public repo, per-recipe quickstart docs. Talker ~500 lines of userspace C; receiver ~250 lines.

**Time estimate:** 3 weekends.

See the detailed M1 plan below.

### M2 — Multichannel PCM, hi-res rates, per-DAC test matrix

**Goal:** Scale beyond stereo/48 kHz to the rest of the PCM space: 5.1 / 7.1 / 7.1.4 channel counts on multichannel USB DACs (MiniDSP MCHStreamer, Motu UltraLite, OKTO Research dac8, Exasound e38, etc.), and hi-res rates (88.2 / 96 / 176.4 / 192 kHz) for stereo audiophile use. Format lock stays at s24le-3 in M2; additional sample widths arrive later.

**Deliverables:**
- `--channels N` and `--rate HZ` on both talker and receiver (matching manually for M2; auto-negotiation arrives in M7 with mDNS-SD / AVDECC).
- MTU check at talker startup; reject configurations whose worst-case packet exceeds 1500 bytes with a clear message. This caps 12ch at 192 kHz and below; stereo goes all the way to the format lock; 16ch fits at 192 kHz with tight margins.
- Mode C constants (NOMINAL_SPM, Q16.16 reference, sanity band) derived from the configured rate rather than hardcoded to 48 kHz.
- `docs/dacs.md` — growing matrix of tested multichannel / hi-res configurations per DAC model.
- Updated recipe docs covering multichannel sources (Roon 5.1 output, UPnP DLNA multichannel content, PipeWire multichannel routes).

**Key risks:** Multichannel USB DAC support on Linux is uneven; some require `snd_usb_audio` quirks or vendor firmware. The test matrix in `docs/dacs.md` is the deliverable that manages this risk — real per-DAC reports rather than vendor-claim parroting.

**Time estimate:** 2 weekends + calendar time to accumulate DAC test reports.

### M3 — Tier 2 hardware, hardware PTP

**Goal:** Bring up same stack on a Tier 2 Linux SBC. Validate sub-µs PTP sync.

**Deliverables:** Receiver runs on at least one Tier 2 platform (i.MX 8M Mini recommended). `ptp4l` with hardware timestamping. Two-talker phase alignment measured.

**Time estimate:** 3–4 weekends, mostly hardware bring-up.

### M4 — IP/UDP transport (WiFi and routed networks)

**Goal:** Add IP/UDP transport (Mode 3) alongside the existing L2 mode so streams work over WiFi, across VLANs, and through routers. Same AoE header inside a UDP datagram.

**Deliverables:** Talker and receiver gain `--transport ip` flag. Unicast UDP works; multicast UDP (for multi-receiver streams) documented and tested for IPv4 and IPv6. DSCP tagging with `EF` for WMM / WiFi QoS. Receiver handles both L2 and IP on the same instance (configurable per stream). End-to-end demo: talker on wired, receiver on Pi connected via WiFi, audio plays cleanly with documented latency and jitter numbers.

**Key risks:** WiFi jitter is highly variable; need to characterize what buffer depth keeps a single-receiver stream glitch-free on typical home WiFi. Multi-receiver phase alignment over WiFi is explicitly not a goal — document clearly.

**Time estimate:** 2–3 weekends including WiFi characterization.

### M5 — AVTP AAF transport (Milan interop)

**Goal:** Add AVTP (Mode 2) for Milan/AVB interop on PCM streams. DSD streams continue to use Mode 1 or 3.

**Deliverables:** Talker emits AVTP AAF for PCM when `--transport avtp` is set. Receiver parses AVTP on the configured interface. Interop test with at least one Milan listener (Hive-controlled Motu AVB interface or equivalent).

**Time estimate:** 2 weekends.

### M6 — Native DSD end-to-end

**Goal:** Native DSD64 through DSD512 working through the full pipeline. DoP as a selectable fallback.

**Deliverables:** Wire format format-code parsing, ALSA native-DSD format selection on receiver, verified audio playback through a native-DSD-capable DAC (Topping D90, Holo May, RME ADI-2, etc.). DoP mode selectable when DAC prefers it. Works over L2 (Mode 1) and IP/UDP (Mode 3); AVTP does not carry DSD.

**Key note:** This milestone is dramatically simpler than it was in earlier drafts because `snd_usb_audio` already does the hard work. Our code is about wire format and format selection, not kernel drivers.

**Time estimate:** 2 weekends including real-DAC testing.

### M7 — AVDECC, discovery, and MCU receiver track kickoff

**Goal:** Discovery and control plane across all transport modes. Start the Tier 3 MCU receiver track in parallel.

**Deliverables:**
- Linux side: AVDECC entity model for L2/AVTP deployments (Hive-compatible). mDNS-SD for IP deployments. Capability records populated from DAC discovery. Stream setup negotiates format + transport end-to-end.
- MCU side: RT1170 USB host stack enumerates a target DAC and streams stereo PCM from Ethernet. L2 mode first; IP mode added opportunistically.

**Time estimate:** 6–8 weekends across tracks.

### M8 — Scale, soak, DSD1024/2048, packaging

**Goal:** Full Atmos on Linux receivers; DSD1024 / DSD2048 on MCU; move from "working prototype" to "deployable streamer."

**Deliverables:**
- Linux: 7.1.4 PCM Atmos bed, 22.2 streams, 24-hour soak, multi-receiver topologies, failure-mode docs.
- MCU: multichannel PCM, native DSD64 through DSD1024, DSD2048 stereo. Per-DAC quirk table for major DSD DACs.
- Linux ALSA: investigate native DSD1024/2048 exposure; propose patches if needed.
- Deployment recipes (listening room, home theater, small venue, audiophile rack).
- Debian packages for talker and Linux receiver; firmware image for MCU receiver.
- Project website with docs and examples.

**Time estimate:** 4–6 weekends across tracks plus calendar time for soak runs.

### M9 — Ravenna / AES67 interop

**Goal:** First-class interop with the Ravenna / AES67 ecosystem via Mode 4 (RTP over UDP). Opens the project to a much larger adjacent market: any AES67 device (Merging Anubis, Neumann MT 48, any Ravenna-native gear, any Dante-with-AES67-mode device, any `aes67-linux-daemon` deployment) becomes a valid talker or listener alongside AOEther-native endpoints.

**Deliverables:**
- **RTP/AES67 encapsulation** for PCM streams: L16 and L24 payload types, 1 ms packet time (PTIME=1 by default, 125 µs optional for low-latency), 48 kHz baseline with optional higher rates.
- **SDP session description** per AES67 conventions, carrying stream format, packet time, channel count, source, destination, and PTP domain info.
- **SAP (Session Announcement Protocol)** for stream discovery, interoperable with AES67 discovery implementations.
- **PTPv2 default profile** support (separate from gPTP which is 802.1AS) for sync with AES67 infrastructure. Linuxptp's `ptp4l` supports both profiles; configuration is per-deployment.
- **Interop test plan:** confirmed playback with at least `aes67-linux-daemon` (open-source reference), and aspirationally one commercial device per ecosystem (a Dante-with-AES67 device, a Ravenna-native device, a Merging or Neumann endpoint).
- **Documented constraints:** AES67 is PCM only, so DSD streams remain on Mode 3 (IP/UDP with AoE header). AES67 channel counts typically cap at 8 per stream; higher counts split across streams.
- **User-facing config** for Mode 4 is kept simple: "I want to send this stream as AES67" / "I want to receive AES67 streams" as first-class options alongside the AOEther-native modes.

**Key risks:**
- AES67 has enough ambiguity in practice that interop edge cases are expected. Approach: start with `aes67-linux-daemon` as a known-good reference, then expand to commercial gear with iteration.
- SAP multicast is sometimes blocked by consumer routers; document workarounds (static SDP files, manual stream addition).
- PTP domain coexistence with gPTP deployments (Milan uses domain 0 typically; AES67 setups often use other domains). Ensure AOEther can participate in both simultaneously if needed.

**Time estimate:** 4–6 weekends.

### Future work — Topology A (gadget mode), redundancy

Deferred to post-M9 "pro track." Topology A gadget mode adds DAW integration (receiver-as-UAC2-device), CBS / TSN shaping hardening, and FRER-style redundancy. See Appendix C for details.

## M1 detailed plan

### Hardware

- **Receiver:** Raspberry Pi 4 Model B (2 GB or 4 GB) or Raspberry Pi 5 (4 GB). Total ~$60 with official power supply.
- **USB DAC:** Anything that works as a UAC2 device on Linux. For bring-up the cheapest acceptable option is a USB headphone dongle (~$20). Mainstream recommendations:
  - AudioQuest DragonFly Black (~$100) — mainstream, well-supported
  - Topping E30 (~$130) — stereo DAC, widely used, DSD-capable (future use)
  - Schiit Modi+ (~$100) — pure PCM, simple
  - RME ADI-2 DAC FS (~$1100) — if you already own one; gold-standard reference
- **Talker:** Any Linux PC with an Ethernet port. For M1 the NIC doesn't matter.
- **Network:** A dumb gigabit switch or a single Cat 6 cable directly between talker and Pi.

### RPi setup (one-time)

Install Raspberry Pi OS (64-bit, Bookworm or newer). Install build dependencies:

```sh
sudo apt install build-essential libasound2-dev alsa-utils
```

Plug the DAC into a USB port. Verify it's recognized:

```sh
aplay -L | grep -A 1 'hw:'
```

You should see the DAC listed, e.g., `hw:CARD=Dragonfly,DEV=0`. Note the card name — the receiver takes this as a command-line argument.

That's the whole setup. No kernel overlays, no configfs scripts, no USB device-mode plumbing. If the DAC shows up in `aplay -L`, the receiver can drive it.

### Wire format for M1

Hardcoded for the audio (data) path:

- Stream ID: `0x0001`
- Format: `0x11` (PCM s24le-3)
- Channels: 2
- Sample rate: 48 kHz nominal
- Payload count: 6 samples/ch/packet nominal; **varies per packet under Mode C feedback** (integer part of the talker's fractional sample accumulator)
- Cadence: 8000 pps via `timerfd` (fixed, does not retune)
- EtherType: `0x88B5`
- Destination MAC: hardcoded at talker; receiver learns source MAC from the first data frame

Mode C control path (receiver → talker):

- EtherType: `0x88B6`
- Frame type: `0x01` (FEEDBACK)
- Feedback value: Q16.16 samples per 1 ms, derived from the receiver's ALSA consumption rate via `snd_pcm_delay()` / `snd_pcm_status()` tracking
- Cadence: ~50 Hz (every 20 ms)
- Stale-feedback fallback: talker reverts to nominal rate after ~5 s of silence
- See [`wire-format.md`](wire-format.md) §"Control frames" for the 16-byte AoE-C header layout

### Talker (`talker/`)

~500 lines of C, depends on libasound2 for the capture source. Shares `packet.{h,c}` with the receiver via `common/`.

```
talker/
├── Makefile
├── README.md
└── src/
    ├── talker.c              — main loop, timerfd, sockets, sample accumulator, feedback RX
    ├── audio_source.h        — abstract source interface
    ├── audio_source_test.c   — 1 kHz sine wave (bring-up, diagnostics)
    ├── audio_source_wav.c    — WAV file loop (bring-up, diagnostics)
    └── audio_source_alsa.c   — ALSA capture (primary music-source path; see docs/recipe-*.md)
```

Additional Mode C responsibilities:

- Open a second `AF_PACKET` socket on EtherType `0x88B6` (non-blocking) for FEEDBACK frames.
- Drain pending feedback once per timer tick; update `samples_per_microframe` from the latest Q16.16 value, clamped to a ±1000 ppm safety band.
- Maintain a Q64 fractional sample accumulator. Each packet's `payload_count` is the integer part of the accumulator; the fractional residual carries into the next packet.
- Vary `sendto()` length per packet: `14 + 16 + payload_count * channels * bytes_per_sample`.
- Revert to nominal rate (48 kHz) if no FEEDBACK has arrived for ~5 s.

Run: `sudo ./build/talker --iface eno1 --dest-mac aa:bb:cc:dd:ee:ff --source testtone`

### Receiver (`receiver/`)

~250 lines of userspace C, depends on libasound2. Shares `packet.{h,c}` with the talker via `common/`.

```
receiver/
├── Makefile
├── README.md
└── src/
    └── receiver.c            — main loop, rate estimator, feedback TX
```

Data-path sketch (Mode C feedback logic elided):

```c
int sock = socket(AF_PACKET, SOCK_RAW, htons(0x88B5));
/* bind to interface */

snd_pcm_t *pcm;
snd_pcm_open(&pcm, dac_name, SND_PCM_STREAM_PLAYBACK, 0);
snd_pcm_set_params(pcm,
                   SND_PCM_FORMAT_S24_3LE,
                   SND_PCM_ACCESS_RW_INTERLEAVED,
                   2 /*channels*/,
                   48000 /*rate*/,
                   0 /*soft_resample off*/,
                   5000 /*latency_us — headroom for Mode C*/);

uint8_t buf[2048];
while (1) {
    ssize_t n = recv(sock, buf, sizeof(buf), 0);
    if (n < 14 + 16) continue;
    aoe_hdr_t *hdr = (aoe_hdr_t*)(buf + 14);
    if (hdr->magic != 0xA0 || hdr->version != 0x01) continue;
    if (hdr->format != 0x11 || hdr->channel_count != 2) continue;
    uint8_t *payload = buf + 14 + 16;
    snd_pcm_writei(pcm, payload, hdr->payload_count);
    /* plus: periodic FEEDBACK frame emission; see receiver/src/receiver.c */
}
```

ALSA is the jitter buffer; `snd_usb_audio` handles UAC2 async feedback with the DAC. The receiver additionally runs a rate estimator — sampling `snd_pcm_delay()` at ~50 Hz and tracking `frames_written − delay` over time to derive DAC consumption rate — and emits a Q16.16 FEEDBACK frame on EtherType `0x88B6` to discipline the talker. See `receiver/src/receiver.c` for the complete loop.

### Build and run

```sh
# Talker
cd talker && make
sudo ./build/talker --iface eno1 --dest-mac $(cat ../pi_mac.txt) --source testtone

# Receiver (on the Pi)
cd receiver && make
sudo ./build/receiver --iface eth0 --dac hw:CARD=Dragonfly,DEV=0

# Listen — plug headphones into the DAC. You should hear a 1 kHz tone.
```

### Test plan for M1

**Functional:**
1. 1 kHz test tone plays cleanly. Spectrum: harmonics below −80 dBFS.
2. WAV file loop replays. Capture on a second Pi with line-in if you want a bit-exactness check.
3. Talker stopped → audio silence (no pop) → talker restart → audio resumes.

**Timing:**
4. Measure end-to-end latency by generating a test click at the talker source and timing its arrival at the DAC output. Target: under 10 ms on a direct Ethernet link (higher than the project-wide 5 ms goal — M1 runs with a deliberately generous jitter buffer to absorb `timerfd` scheduler jitter).
5. CPU load on Pi in steady state. Target: under 10% on a single core.

**Clock discipline (Mode C):**
6. 1-hour test tone with receiver logging ALSA buffer fill level at 1 Hz. Fill level should stay within a bounded band (±10 ms worth of samples) across the full run, not drift monotonically.
7. Positive control: run with `--no-feedback` on the receiver (omits FEEDBACK emission). System must drift and xrun within minutes, confirming the feedback loop is doing real work in (6).

**Soak:**
8. 1-hour test tone, zero audible glitches, zero ALSA underruns reported.

### Deliverables checklist for M1

- [ ] Talker compiles on Ubuntu 22.04 / 24.04.
- [ ] Receiver compiles on Raspberry Pi OS Bookworm.
- [ ] Mode C feedback loop working: 1-hour soak with bounded buffer fill (test 6) and a positive control via `--no-feedback` (test 7).
- [ ] ALSA capture source working: talker plays back a real music stream from at least one of {Roon, UPnP MediaRenderer, PipeWire/Pulse system capture} through to the receiver's USB DAC.
- [ ] `README.md` with BOM, setup, "first audio in 30 minutes" quickstart.
- [ ] `docs/design.md` (this doc).
- [ ] `docs/wire-format.md` including §"Control frames".
- [ ] `docs/recipe-roon.md`, `docs/recipe-upnp.md`, `docs/recipe-capture.md` bridge recipes.
- [ ] `CONTRIBUTING.md`.
- [ ] GitHub Actions CI.
- [ ] Short demo video.

### Known deferred items from M1

- No gPTP. Clock discipline is Mode C only (UAC2-extended feedback); gPTP-disciplined emission (Mode B, for multi-receiver phase alignment) arrives in M3.
- `timerfd` cadence with millisecond-class jitter under load. The generous jitter buffer absorbs it.
- No capability discovery; hardcoded format.
- No stream setup / control plane beyond FEEDBACK frames.
- Single audio direction (talker → receiver); only FEEDBACK frames travel the other way.
- No authentication, no encryption.
- PCM only; DSD arrives in M6.
- L2 Ethernet transport only (Mode 1) for both audio and control frames; WiFi / IP arrives in M4.

## Open questions

**Both topologies eventually, or Topology B only?** Plan: both. Topology A as post-M8 future work (Appendix C). Topology B alone misses DAW integration use cases that Milan serves well.

**Which Tier 2 platform for first bring-up?** Probably i.MX 8M Mini. Decide at M3 kickoff.

**Discovery: AVDECC vs mDNS-SD vs SAP?** All three eventually. AVDECC for Milan (Mode 2, M7), mDNS-SD for simple home IP use (Mode 3, M7), SAP for AES67 interop (Mode 4, M9).

**PTP profile?** gPTP (IEEE 802.1AS-2020) on wired. PTPv2 over UDP for IP mode. Best-effort software PTP over WiFi.

**Reverse direction (USB → network, microphone-style)?** Defer to future work. Same architecture inverted: receiver becomes talker, DAC becomes ADC, samples flow the other way. Most code reusable.

**DSD1024/DSD2048 on ALSA today?** Unclear without testing. `DSD_U32_LE` covers up to DSD128 bit-packing; very-high-rate DSD may need kernel/driver work. Resolve during M6/M8.

**UDP port assignment for Mode 3?** Use port 8805 as an interim value. Submit an IANA request once the project is public and the protocol stabilizes.

**WiFi timing target?** For Mode 3 over WiFi, commit to a documented accuracy target (e.g., "single-receiver jitter under 5 ms, no inter-receiver phase alignment guarantee"). Revisit if FTM-based timing becomes commodity.

## Appendix A: Reference implementations to study

- **Linux `snd_usb_audio`** (`sound/usb/` in the kernel tree), particularly `quirks-table.h` — authoritative reference for how native DSD is handled per-DAC. Read this before touching DSD in M6.
- **`linuxptp`** — `ptp4l`, `phc2sys`, `pmc`. Used from M3.
- **OpenAvnu** — AVTP / AVDECC reference, though heavyweight.
- **Hive** — open-source AVDECC controller, validates M7.
- **HQPlayer NAA protocol** (closed but documented behavior) — reference for what audiophile-grade network audio receivers do in Topology B.
- **Sonore / Allo / SOtM product docs** — reference architectures for commercial Topology B streamers.
- **TinyUSB** — MCU USB host stack that could be the Tier 3 starting point, alongside NXP's USB host stack.
- **NXP MCUXpresso SDK** — `usb_host_audio` examples for the RT1170 USB host path.
- **`aes67-linux-daemon`** — open-source AES67 receiver/transmitter. Reference for Mode 4 (AES67 / RTP) interop in M8.
- **Ravenna documentation** (ALC NetworX) — AES67 profile details, SDP session format, SAP announcement format.
- **IEEE 802.11 Timing Measurement / FTM** — IEEE 802.11mc specifications; potential future path to improved WiFi timing.

## Appendix B: Key design decisions

**Why Topology B primary?** The v0.3 plan assumed the receiver was a USB gadget presenting UAC2 to an upstream host. Two problems: (1) Native DSD requires `f_uac2` patches because stock `f_uac2` is PCM-only. (2) Gadget mode adds a whole host computer to the downstream chain, which most audiophile deployments don't want. Topology B puts the DAC directly on the receiver's USB port, leverages `snd_usb_audio`'s mature native-DSD support (DSD1024 today, no patches), and matches the natural "network streamer" product shape. Topology A remains compelling for pro-AV / DAW integration and is preserved as post-M8 future work (Appendix C).

**Why native DSD primary, not DoP?** In Topology B there is no `f_uac2` gap to work around, so the legacy DoP path has no technical advantage. Native DSD is what DSD-capable DACs natively support, what audiophile workflows use, and has no bandwidth overhead or rate ceiling. DoP survives as a compatibility format code for DACs or hosts that prefer it.

**Why a custom wire format for M1 instead of AVTP?** AVTP parsing adds complexity that M1 doesn't need. The format-code abstraction in our header extends naturally to AVTP AAF (M5) for PCM and keeps DSD in our own subtype since AVTP AAF doesn't carry DSD.

**Why multiple transport modes instead of one?** Different deployments have different constraints. Raw Ethernet (Mode 1) gives us the tightest timing for wired TSN-style setups. AVTP (Mode 2) unlocks the Milan ecosystem. IP/UDP (Mode 3) reaches WiFi and routed networks where the majority of consumer audio lives today. RTP/AES67 (Mode 4) opens interop with the Ravenna/AES67 pro-AV-over-IP ecosystem. A single-transport design would foreclose at least one of these communities. The cost of supporting all four is modest because the header and payload are identical across modes — only the wrapper changes.

**Why defer Topology A?** The audience that needs gadget mode (pro-AV, DAW integration) is served well by existing Milan-certified gear. The audience that doesn't have a good open-source option (audiophile DSD streaming to USB DACs, now including WiFi) is served by Topology B. Starting with the underserved audience builds a user base faster; Topology A can layer on when Topology B is solid.

**Why Apache 2.0?** Pro AV vendors are commercial; GPL blocks their contributions. Patent grant matters for TSN-related patents.

**Why three hardware tiers?** Single-tier forces bad tradeoffs. RPi for cheap entry, Tier 2 for hardware PTP / Milan, MCU for embedded product form factor.

## Appendix C: Topology A (gadget mode) — future work

Topology A defers to post-M9 future work, but preserving the design rationale here so we can execute when we get there.

**Use case.** A computer (DAW, media server, user's laptop) plugs into the receiver via USB. The receiver presents as a UAC2 device — either an audio input (the host records audio from the network) or output (the host plays audio to the network, reverse direction). This is how Milan endpoints integrate with DAW workflows today.

**Hardware.** Tier 1/2 use the receiver's USB-C port (Pi 4/5) or equivalent device-mode-capable port in gadget mode. Tier 3 MCU uses its USB OTG controller in device mode. Same DAC-side hardware (if any) is irrelevant in Topology A because the DAC is downstream of the computer, not of the receiver.

**Software.** Tier 1/2 uses Linux's `libcomposite` + `f_uac2` gadget driver. PCM works out of the box; native DSD requires `f_uac2` patches (Format Type III alt settings + per-project `quirks-table.h` entry). Tier 3 MCU implements UAC2 device stack directly — an easier path for native DSD since we control descriptors.

**What changes in our code:**
- Talker: no changes.
- Protocol: no changes.
- Receiver: an alternative output path that writes samples into `f_uac2`'s ALSA device instead of a DAC's ALSA device. Mostly a config switch.

**Kernel work for native DSD in Topology A on Linux:**
- Add UAC2 Format Type III alternate settings to `f_uac2.c`
- Register a project VID/PID in `quirks-table.h` with native DSD entries
- Estimated ~300–500 lines upstreamable kernel patch.

This work is post-M8 future work but could be an early stretch goal if a contributor wants to take it on — particularly if someone wants DAW integration before the main roadmap reaches it.

## Appendix D: Risk register

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| DAC-specific quirks in `snd_usb_audio` don't cover some target DACs | Medium | Medium | Maintain a "tested DAC list" per milestone; contribute quirk entries upstream when needed |
| Tier 2 hardware bring-up takes longer than estimated | High | Medium | Pick one platform (i.MX 8M Mini) and commit; defer others |
| Native DSD1024/2048 not supported by current ALSA DSD format codes | Medium | Low | Investigate during M6; propose ALSA patch if needed; MCU tier unaffected |
| Milan AVDECC interop edge cases discovered late | Medium | High | Read Milan spec during M2; engage Avnu Alliance early; attend a plugfest as observer |
| MCU USB host stack performance limits for DSD2048 multichannel | Medium | Low | DSD2048 multichannel is already gigabit-bound; document supported DAC/channel combos per tier |
| WiFi jitter too variable for usable single-receiver playback | Medium | Medium | Characterize during M4; document recommended buffer depth per WiFi class (5 GHz vs 2.4 GHz, mesh vs single AP); fall back to larger buffers rather than rejecting WiFi |
| AES67 interop (M9) blocked by RTP/SDP/SAP edge cases | Medium | Medium | Start with `aes67-linux-daemon` as reference; accept that commercial-gear interop may require multiple iterations; budget real time in M9 for commercial-gear testing |
| PTP domain conflicts when hosting Milan (gPTP) and AES67 (PTPv2) on one network | Medium | Low | Document per-deployment PTP domain selection; AOEther talker supports multiple domains where hardware allows |
| `f_uac2` kernel patch for Topology A rejected upstream | Low | Low | Topology A is post-M9 future work, not critical path; maintain out-of-tree if needed; DoP works without the patch |
| Contributors don't materialize | Medium | Medium | Working demo on a $60 Pi + a $20 USB DAC before announcement |
