# Audio-over-Ethernet for USB DACs

**Status:** Draft v1.5 — M1–M4 merged to main; M5 (AVTP AAF) and M6 (native DSD64–DSD512) code complete on their feature branches
**Audience:** Contributors, reviewers, early adopters
**License:** GNU GPL-3.0-or-later

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

Object-based Atmos rendering is out of scope; channel-based multichannel covers bed formats up to 22.2 and object rendering belongs in a separate layer. Compressed formats (AC-4, Dolby TrueHD, DTS, MQA) are out of scope. Effects, mixing, or any DSP is out of scope. Continuous sample-rate conversion is out of scope — neither the talker nor the receiver runs a resampler in the audio data path. The one exception, bounded and explicit, is hold-last-sample tail-fill at the talker's ALSA capture edge: when an upstream source (gmrender, MPD, a DAW bridged via snd-aloop) cannot match the DAC-driven consumption rate on the far side, the capture read's missing tail is repeated from the last valid frame. At typical consumer DAC crystal spreads (±20 ppm) this resolves to ~20 µs plateaus whose audibility sits below the noise floor of any real listening environment — it is nearest-neighbour tail-repeat, not running-rate SRC. The bound is controllable via `--capture-buffer-ms` on the talker, which sets the floor on how often fills occur during transient drift events. See §"Clock architecture" for the full chain. **Milan-grade timing over WiFi is out of scope** — consumer WiFi lacks hardware PTP and TSN; the IP/UDP transport mode works over WiFi with documented best-effort timing for single-receiver use cases, not for multi-receiver phase-aligned deployments. **Topology A (gadget mode / UAC2 device emulation) is deferred**, not rejected — it's a compelling feature for pro-AV and DAW integration that belongs on a later milestone once the core is stable.

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

The talker accepts audio from any source (ALSA capture, JACK client, file, synthesized test signal), packages samples into packets per the agreed stream format, and emits them on a cadence aligned to USB microframes. The receiver enumerates its attached DAC, discovers what the DAC supports, advertises those capabilities to the talker via the control plane, and then forwards incoming samples to the DAC through the host OS's UAC2 driver. No format conversion, no continuous SRC — if the talker and DAC disagree on format, stream setup fails cleanly. Hold-last-sample tail-repeat at the talker's capture edge absorbs source-vs-DAC rate drift for sources that can't rate-match themselves; see §"Goals / Non-goals" for the bounds.

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

### What happens when the upstream source can't rate-match

Mode C propagates the DAC's clock from the receiver back to the talker's emission, and from the talker back to the ALSA capture device it reads from — typically a `snd-aloop` ring fed by gmrender, MPD, a DAW, a PipeWire virtual sink, or similar. When the upstream source is itself adaptive (e.g., RoonBridge, a native ALSA app using blocking writes), that source follows the back-pressure and the whole chain runs at DAC rate. When the upstream is rate-fixed (gmrender playing a FLAC file: 48000 samples per "second of audio," produced at 48000.0 samples per wallclock second), the source *cannot* produce samples faster than its nominal rate, and a positive-drift DAC asks for more than the source can deliver. Something has to absorb the delta.

AOEther absorbs it with **hold-last-sample tail-repeat at the capture edge**: when the talker's `snd_pcm_readi()` returns fewer frames than the current packet requires, the missing tail is filled with the previously-read frame. This is *not* running-rate SRC — there is no kernel-continuous interpolation or bandwidth-limiting filter — it is nearest-neighbour repeat, invoked only in the instantaneous fraction where the ring ran dry. At steady state under 20 ppm positive DAC drift the repeat duty is ~0.002% (roughly one sample per second of playback, after any buffered slack has drained); ~20 µs of waveform plateau at 1 Hz is below the threshold of any plausibly-detectable artifact on real program material. `--capture-buffer-ms` on the talker (default 100 ms; the DLNA recipe recommends 200 ms) controls the ALSA capture ring depth, which bounds how quickly the system reaches that steady state after a transient event (a track change that refills gmrender's pipeline, a pause/resume, etc.).

The point of the exception is architectural honesty: extending UAC2 async feedback across Ethernet handles the *network* segment precisely, and the *network* segment is what Mode C was designed to keep drift-free. Beyond the talker's capture edge, nothing in the system can speak the feedback protocol to a rate-fixed file source — and rather than either silently resampling (violates the project's bit-exact ambition) or letting the receiver glitch audibly (violates the project's reason to exist), we absorb the irreducible delta with a tail-repeat that is audibly indistinguishable from no repeat at all. Hold-fill is never used in place of silence; it's used *because* silence inserts an audible transient and hold-fill does not.

Stats on hold-fill events are logged at talker shutdown (`held_fill_frames`, `held_fill_events`) so operators running continuous long-session playback can confirm the artifact rate matches the math.

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

Overruns drop oldest slot. Packet loss and underruns are concealed by the PLC envelope described below. Both report to the control plane for monitoring but never block the data path.

### Packet-loss concealment (PLC)

The data path carries no retransmission — raw L2, UDP, AVTP, and RTP all lose packets silently — so the receiver must decide what to hand the DAC for the missing frames. The naive choices are both bad:

- **Zero-fill.** Produces two discontinuities bracketing the gap (a cliff from the last real sample to 0 on entry, another from 0 to the first real sample on exit). Audible as clicks on any signal that wasn't already near a zero-crossing.
- **Pure sample-and-hold.** Continuous on entry (one click moved to exit, net improvement), but a held peak-level sample sent through a DC-coupled amp to a tweeter for hundreds of milliseconds is a real risk if the stream never resumes.

So the receiver runs a three-phase envelope that combines the strengths of both. It is deliberately **not DSP on the music** — the synthesis only runs during gaps that weren't in the music to begin with, and touches zero samples of legitimate audio except for the brief ramp-back-in scaling described below.

1. **Hold-last** for up to `--plc-hold-ms` (default 1 ms at 48 kHz ≈ 48 frames). Per-channel, repeat the last successfully-written PCM sample. For gaps shorter than the hold window this is the entire synthesis — one discontinuity on exit, bounded by max signal excursion, same as zero-fill's single-side click.
2. **Linear ramp to zero** over the next `--plc-ramp-ms` (default 5 ms). Multiplies the held sample by a linear gain that steps from 1.0 down to 0.0. Puts the exit discontinuity at a guaranteed-zero value and spreads it over a raised-level edge rather than a cliff.
3. **Zero** for the rest of the gap. DC-coupled tweeter protection: no held non-zero value persists on a long outage.

When real packets resume, the first `ramp-ms` of legitimate audio is **ramp-back-in scaled** — gain goes 0.0 → 1.0 over the ramp window, then identity. This is the one place the envelope touches real samples, and it's necessary because otherwise the exit-from-zero at packet resume would itself be a discontinuity. The envelope is multiplicative; no resampling, no filtering, no per-sample prediction.

Policy knobs (`--plc hold_ms,ramp_ms` on the receiver; `--plc off` disables):

- **Default 1 ms hold, 5 ms ramp.** At 48 kHz that's 48 held samples + 240 ramp samples. Covers a single lost packet (~125 µs in Mode C at 8000 pps) entirely as hold, and hides a 1–5 packet burst as ramp-to-silence.
- **Max synthesized gap ~100 ms.** Beyond that, `snd_pcm_writei` is permitted to underrun naturally; Mode C will re-bootstrap the rate estimator when real packets resume. PLC is click-avoidance, not error correction.
- **PCM only.** DSD's 1-bit stream doesn't admit linear scaling — a held non-zero bit pattern is DC at the DAC's analog filter output, which is safe *enough*, but a ramp of a 1-bit value is meaningless. The DSD path falls back to the DSD-idle byte `0x69` (defined in `packet.h` as `AOE_DSD_IDLE_BYTE`) for the gap duration. No ramp.
- **Last-sample state resets on ALSA xrun recovery.** After `snd_pcm_prepare` the buffer is empty and replay resumes from the next real packet; carrying the old last-sample across an xrun would reintroduce the exact click the envelope exists to avoid.

**Non-goals.** No linear predictive concealment (G.711 Appendix I style). No pitch-detected waveform extrapolation (Opus/WAV-interp style). Those are per-sample DSP, explicitly disallowed by §Goals. The envelope here is scalar-gain-over-frame and nothing more.

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

**Goal:** Bring up the same receiver stack on a Tier 2 Linux SBC with hardware-PTP-capable Ethernet. Validate sub-µs `ptp4l` sync. Lay the plumbing for gPTP-disciplined emission (Mode B) and multi-receiver phase alignment.

M3 is split into two sub-phases because it's mostly hardware work:

**Phase A — docs-ready (can land without hardware):**
- `docs/tier2-bringup.md` — reference platform choice (i.MX 8M Mini), OS choice (Debian 12 arm64 or Yocto), kernel/driver confirmation steps, first-boot checks that exercise the existing M1/M2 receiver on Tier 2 without PTP.
- `docs/ptp-setup.md` — ptp4l + phc2sys configuration for gPTP (802.1AS) and PTPv2-default profiles; validation against a reference; systemd units for persistent operation; software-fallback mode for Tier 1.
- No code changes. M1/M2 already runs on Tier 2 as a plain arm64 Debian application — that's a nice property of the Topology B choice.

**Phase B — hardware-required (lands after someone has a Tier 2 board running ptp4l):**
- Talker `--ptp-clock` / `--presentation-time-offset-ms` flags. When set, `clock_gettime(CLOCK_TAI)` on each emission, stamp `presentation_time = low32(tai_ns + offset_ns)`. Default path unchanged.
- Receiver `presentation_time`-aware scheduling. Compute target playback time; arrange `snd_pcm_writei()` to hit that target within ALSA's achievable resolution.
- Statistics on presentation-time arrival jitter for debugging.
- Measured two-receiver phase alignment on a wired gigabit LAN with a scope; target < 10 µs skew.

**Deliverables:** Both phases. Phase A merges independently; Phase B merges once validated on real hardware.

**Key risks:** Tier 2 hardware bring-up timeline depends on board availability, not on AOEther code complexity. Managed switches often drop PTP silently — documented in `ptp-setup.md`.

**Time estimate:** Phase A 1 weekend (doc writing). Phase B 2–3 weekends once a Tier 2 board is in hand, dominated by ptp4l/phc2sys tuning and scope-based measurement.

### M4 — IP/UDP transport (WiFi, routed networks, multicast)

**Goal:** Add IP/UDP transport (Mode 3) alongside the existing L2 mode so streams work over WiFi, across VLANs, and through routers. Same AoE header inside a UDP datagram. Both IPv4 and IPv6. Both unicast and multicast. Same `snd-aloop` bridge recipes keep working — only the transport wrapper changes.

**Deliverables:**
- Talker and receiver gain `--transport l2|ip`, `--port` flags. Talker adds `--dest-ip` accepting either IPv4 or IPv6 literals. Receiver adds `--group IP` for multicast membership.
- IPv4 + IPv6 unicast and multicast working end to end. Address family auto-detected from the literal; multicast auto-detected from the address range (`224.0.0.0/4` or `ff00::/8`).
- DSCP tagging with Expedited Forwarding (0xB8) via `IP_TOS` / `IPV6_TCLASS`. Advisory — improves WMM AC_VO handling on WiFi and DSCP-aware wired switches.
- Talker-side Mode C arbitration over multiple FEEDBACK sources (up to 16 receivers): take the **slowest** non-stale rate so no receiver xruns. Per-source sequence numbers track independently.
- Single-socket IP design: talker uses one UDP socket for TX data + RX feedback; receiver uses one for RX data + TX feedback. Magic byte (`0xA0` vs `0xA1`) disambiguates.
- End-to-end demo: talker on wired, receiver on Pi connected via WiFi, audio plays cleanly with documented latency and jitter numbers. Second demo: talker + two receivers on IPv4 multicast, both playing in lockstep.

**Key risks:** WiFi jitter is highly variable; need to characterize what buffer depth keeps a single-receiver stream glitch-free on typical home WiFi. Multi-receiver phase alignment over WiFi is explicitly not a goal (that needs gPTP, which WiFi can't do) — document clearly. Multicast through consumer home switches sometimes gets flooded as broadcast; mention as a known limitation in recipes.

**Time estimate:** 2–3 weekends including WiFi characterization and multicast testing through at least one consumer switch.

### M5 — AVTP AAF transport (Milan interop)

**Goal:** Add AVTP (Mode 2) for Milan/AVB interop on PCM streams. DSD streams continue to use Mode 1 or 3.

**Status:** Code complete; interop test pending hardware access to a Milan listener.

**Deliverables:**
- `--transport avtp` on talker emits IEEE 1722-2016 AVTP AAF frames at EtherType `0x22F0` for any AOEther-supported PCM rate (44.1/48/88.2/96/176.4/192 kHz, 1..1023 channels).
- `--transport avtp` on receiver parses AAF and writes to ALSA. Format / NSR / channels / bit-depth must match CLI; mismatched frames are dropped.
- AAF byte-order conversion (big-endian on the wire ↔ ALSA little-endian) handled in-place by `common/avtp.c::avtp_swap24_inplace`.
- Mode C feedback unchanged: AOEther's `0x88B6` control frames continue to flow alongside AVTP data; Milan-only listeners ignore the unknown EtherType.
- Wire-format documentation in `docs/wire-format.md` §"Mode 2 (AVTP AAF)" with full byte layout, NSR table, and sample-byte-order rule.
- Operator recipe in `docs/recipe-milan.md` covering Wireshark AVTP-dissector verification and listener-side stream subscription.

**Out of scope (deferred to M7):**
- AVDECC entity model (ATDECC over IEEE 1722.1) — required for Milan controllers like Hive to *discover* the stream automatically. Without it, listener-side stream ID and talker MAC must be configured manually.
- MSRP / SRP stream reservation. AOEther emits at line rate without per-class shaping; works on quiet wires and TSN-class switches that accept best-effort AAF traffic.
- gPTP-disciplined `avtp_timestamp`. M5 emits `tv=0` and `avtp_timestamp=0`. Listeners that require a valid presentation time will reject; many do not. Real PTP integration arrives in M3 Phase B (hardware) and is wired into the AVTP timestamp here in M7.

**Key risks:** Milan listeners vary in strictness about AVDECC pre-negotiation. Without M7's AVDECC support, some won't accept AOEther's stream regardless of how correct the AAF bits are. The recipe doc calls this out and recommends Hive's "manual stream connection" workflow for testing.

**Time estimate:** 2 weekends for the code (done); interop verification depends on listener access.

### M6 — Native DSD end-to-end

**Goal:** Native DSD64 through DSD512 working through the full pipeline. DoP as a selectable fallback.

**Status:** Wire-format and code paths complete; real-DAC listening test pending hardware access.

**Deliverables:**
- Format codes `0x30..0x32` (native DSD64/128/256) defined in `common/packet.h` and wired through talker and receiver. DSD512 (`0x33`) and up remain reserved in the wire-format table but are not produced/accepted by talker/receiver until M8 — they need packet splitting because the per-microframe byte count exceeds the u8 `payload_count` field.
- Talker `--format dsd64|dsd128|dsd256` switches payload semantics to DSD bytes/microframe (integer alternation around the non-integer target, e.g., 44/45 for DSD64) with no other plumbing changes — the same fractional accumulator that drives PCM payload_count also drives the DSD byte count, so Mode C works unmodified.
- Talker built-in DSD silence source (`--source dsdsilence`, default when `--format` is DSD) emits the `0x69` idle pattern. Enough to verify the wire path and ALSA format selection end to end; real file playback comes in M8.
- Receiver `--format` + `--alsa-format` covers the full matrix of DAC quirks: `dsd_u8` (default, 1:1 passthrough), `dsd_u16_be` / `dsd_u16_le`, `dsd_u32_be` / `dsd_u32_le`. For wider-than-wire formats the receiver deinterleaves wire bytes into per-channel streams (carrying up to N-1 bytes per channel across packet boundaries), repacks at ALSA's N-byte-per-channel granularity, and byte-reverses within each group for the `_le` variants.
- Mode C clock discipline continues to work under DSD rates (tested in code path; rate numbers are ~352.8..1411.2 samples/ms, outside UAC2 HS feedback legal range but AOEther treats the Q16.16 value as opaque rate telemetry).
- Both L2 (Mode 1) and IP/UDP (Mode 3) transports carry DSD; AVTP (Mode 2) rejects DSD at startup per IEEE 1722 AAF's PCM-only scope.
- Operator recipe in [`docs/recipe-dsd.md`](recipe-dsd.md) with smoke-test procedure, a DAC → `--alsa-format` mapping table, troubleshooting, and a clear statement of what's deferred.

**Out of scope for M6 (tracked as follow-ups):**
- DoP encoder on the talker (format codes `0x20..0x23`). The wire format already reserves them; wiring up a PCM-wrapped-DSD source needs a small DoP modulator which is straightforward but didn't make M6's code budget.
- DSF file reader shipped as a post-M6 follow-up (`talker/src/audio_source_dsf.c`; see `docs/recipe-dsd.md` §"Step 3"). Handles the Sony spec's block-per-channel interleave and LSB-first per-byte bit order (AOE wire is MSB-first, so each byte is bit-reversed on read). DFF remains deferred — the per-DAC quirk testing it enables is better concentrated in M8 alongside DSD512+.
- DSD512 / DSD1024 / DSD2048. DSD512 overflows the u8 `payload_count` field; DSD1024 stereo additionally breaks the 1500-byte MTU. Packet splitting + `last-in-group` reassembly arrives in M8.

**Key note:** This milestone is dramatically simpler than it was in earlier drafts because `snd_usb_audio` already does the hard work. Our code is about wire format and format selection, not kernel drivers.

**Time estimate:** 2 weekends for wire-format plumbing + silence-path smoke test + DSD_U8/U16/U32 variants (done). DAC-matrix build-out (M8 overlap) and DSD512+/DSF-reader/DoP-encoder add incrementally.

### M7 — Discovery, AVDECC, and MCU receiver track kickoff

**Goal:** Discovery and control plane across all transport modes. Start the Tier 3 MCU receiver track in parallel. Large milestone, split into three phases that ship independently.

#### Phase A — mDNS-SD discovery (in progress)

Simple-home-network path: receivers publish themselves as `_aoether._udp` with TXT records describing their DAC capabilities (channels, rate, format, transport, port). Talkers and operators discover receivers without static MAC/IP configuration.

- `common/mdns.c` wraps Avahi's `avahi_threaded_poll` API. Graceful no-op fallback when libavahi-client isn't present at build time, so the data path keeps working on minimal systems.
- Receiver: `--announce [--name NAME]` flags publish on startup.
- `tools/aoether-browse` is a small helper that resolves the service type and prints a one-line summary per receiver; `avahi-browse -r _aoether._udp` works as a manual alternative.
- TXT schema and recipe live in [`docs/recipe-discovery.md`](recipe-discovery.md).

**Out of scope for Phase A:**
- Talker-side auto-select (picking a receiver from the discovered set). A `--dest=mdns:NAME` shorthand is a follow-up; for now browse output is piped into the talker invocation manually.
- Expressing full capability matrices in TXT records (e.g. "PCM up to 192 kHz **and** DSD256"). Each run publishes one concrete configuration; capability matrices are Phase B's job.

#### Phase B — AVDECC entity model via la_avdecc

Milan-controller path. Linux side ships an IEEE 1722.1 entity backed by [L-Acoustics' open-source la_avdecc library](https://github.com/L-Acoustics/avdecc) so Hive and other Milan controllers can discover AOEther endpoints and establish streams without manual stream ID / MAC entry.

**Why la_avdecc:** it is the reference implementation Hive itself is built on, handles the Milan/ATDECC state machines correctly out of the box, and saves AOEther from owning the protocol-quirk surface. Tradeoff: it is C++17 (CMake, libpcap, Boost), which makes AVDECC the first C++ dependency in an otherwise pure-C codebase. We keep the main binaries C11 by pushing the library into a small static archive (`avdecc/build/libaoether_avdecc.a`) and exposing a C-shaped wrapper in `common/avdecc.{h,c}`. Receiver and talker Makefiles detect the archive and link it when present; without it `--avdecc` prints a helpful error and the data path keeps working.

Ships in small steps so reviews stay tractable:

1. **Step 1 — scaffolding (shipped).** Submodule wired, CMake build glue produces the static archive, C wrapper in place, `--avdecc` flag on both binaries, recipe doc.
2. **Step 2 — ADP advertising (shipped).** `avdecc/src/entity.cpp` opens a PCap-backed `ProtocolInterface`, creates an `AggregateEntity` with entity name + group name + firmware version, and enables ADP advertising with `listenerStreamSinks=1` (listener) or `talkerStreamSources=1` (talker). Hive's controller pane renders the entity row.
3. **Step 3 — full descriptor tree (shipped).** Configuration tree gains STREAM_INPUT (listener) or STREAM_OUTPUT (talker) at 48 kHz / 24-bit / stereo AAF, parented under AUDIO_UNIT with STREAM_PORT + AUDIO_CLUSTER, plus AVB_INTERFACE (with the real iface MAC), CLOCK_SOURCE (Internal), and CLOCK_DOMAIN. Hive enables Connect on the stream; protocol-level ACMP CONNECT succeeds and la_avdecc updates the stream's internal connection state.
4. **Step 4 — ACMP CONNECT_TX/RX drives the AOEther data path (shipped).** A `ProtocolInterface::Observer` inside the entity watches raw ACMP PDUs and fires `on_bind(peer_mac, stream_id)` / `on_unbind()` into the receiver and talker main loops. Receiver side: ACMP-learned talker MAC is preferred over the first-frame-learns fallback; talker side: ACMP-sourced dest MAC overrides `--dest-mac` on per-packet egress, and `--dest-mac` is no longer required when `--avdecc` is set. Clicking Connect in Hive produces audio end-to-end.
5. **Step 5 — capability matrix in descriptor tree (shipped).** Supported stream formats expand to the set `{48 kHz, 96 kHz, 192 kHz} × --channels × 24-bit AAF` — all three rates at the currently configured channel count.  AUDIO_UNIT `samplingRates` matches.  `currentSamplingRate` / `dynamicModel.streamFormat` reflects `--rate` when it falls in the AAF-compatible set (else defaults to 48 kHz for the descriptor while the data path follows CLI verbatim).  Controllers can now pick any advertised rate; runtime format switching via AECP `SET_STREAM_FORMAT` / `SET_SAMPLING_RATE` is deferred — a controller-chosen format only takes effect if it matches the CLI pin.

**Out of Phase B, tracked:**
- Runtime format reconfiguration: AECP SET_STREAM_FORMAT / SET_SAMPLING_RATE driving live ALSA reopens + fractional-accumulator retune. Belongs with the format-negotiation work in M8.
- 44.1 kHz-family rates over AAF. AAF's 8 kHz SDT cadence wants integer samples-per-frame; 44.1-family gives fractional. Milan devices mostly cap at 48-family anyway.

**Out of scope for Phase B (tracked as follow-ups):**
- MSRP stream reservation. Deferred to the TSN hardening track.
- gPTP-disciplined `avtp_timestamp`. Requires M3 Phase B hardware PTP; drops in once both are merged.
- AECP AEM checksums beyond what Hive requires for basic display.

#### Phase C — MCU receiver track kickoff

RT1170 USB host stack enumerates a target DAC and streams stereo PCM from Ethernet. L2 mode first; IP mode added opportunistically. Shares `common/` with the Linux binaries (packet framing, AAF header, mDNS stub). Needs real NXP MIMXRT1170-EVKB hardware; deliverable is a blinking-LED-plus-stereo-PCM firmware image and a bring-up doc, not a polished streamer — that arrives in M8.

**Time estimate:** Phase A: 1 weekend (shipped). Phase B: 3–4 weekends. Phase C: 2–3 weekends once EVKB arrives.

### M8 — Scale, soak, DSD1024/2048, packaging

**Goal:** Full Atmos on Linux receivers; DSD1024 / DSD2048 on MCU; move from "working prototype" to "deployable streamer."

Ships in pieces as sub-features land rather than as one atomic milestone. Items below are marked **[shipped]**, **[open]**, or **[hardware-blocked]** to reflect the actual state.

**Wire-format / talker / receiver:**
- **[shipped]** *Packet splitting for DSD512+ and high-rate multichannel PCM.* Talkers split a microframe whose per-channel `payload_count` exceeds the `u8` limit (255) or whose total payload exceeds MTU into K fragments emitted back-to-back, with per-fragment monotonic sequence numbers, shared `presentation_time` / `stream_id`, and `flags.last-in-group=1` only on the final fragment. Receivers require no group-level reassembly state — each fragment is a standalone AoE data frame that writes to ALSA unmodified, and per-fragment sequence gaps feed the existing loss counter. AVTP AAF keeps its single-packet-per-microframe constraint; fragmentation applies only to Mode 1 / Mode 3. See `wire-format.md` §"Cadence and fragmentation".
- **[shipped]** *DSD512 / DSD1024 / DSD2048 talker and receiver support.* `--format dsd512 | dsd1024 | dsd2048` (wire codes `0x33`/`0x34`/`0x35`) is accepted on both sides now that packet splitting is in place. At stereo the wire splits DSD512 into 2 fragments/microframe, DSD1024 into 3, DSD2048 into 6. Receiver-side playback depends on the DAC's `snd_usb_audio` quirk entry — modern DSD1024 DACs are already wired up in mainline Linux; DSD2048 DAC availability is narrower but the wire path is live.
- **[open]** *Runtime format reconfiguration via AECP `SET_STREAM_FORMAT` / `SET_SAMPLING_RATE`.* Phase B tracked this here. Needs a live ALSA reopen + fractional-accumulator retune.
- **[shipped]** *DFF (`.dff`) file reader on the talker.* `--source dff --file path.dff` in `talker/src/audio_source_dff.c`. DSDIFF 1.5 uncompressed (`DSD ` CMPR) only; DST-compressed files are rejected. DSDIFF's byte-interleaved MSB-first layout already matches the AOE wire format, so `read()` memcpys straight from the mmap. Supported rates match the DSF reader: DSD64 through DSD2048.
- **[open]** *DoP encoder.* Format codes `0x20..0x23` are wire-reserved; wiring up a PCM-wrapped-DSD source is a modest talker-side addition.

**Hardware-dependent tracks:**
- **[hardware-blocked]** 7.1.4 PCM Atmos bed, 22.2 streams, 24-hour soak, multi-receiver topologies, failure-mode docs. Packet splitting unblocks 22.2 @ 192 kHz wire-format-wise; validation wants multichannel DAC hardware.
- **[hardware-blocked]** MCU: multichannel PCM, native DSD64 through DSD2048. Per-DAC quirk table for major DSD DACs.
- **[hardware-blocked]** Linux ALSA: investigate upstream quirk-entry coverage for DSD1024/2048; propose patches for any DACs mainline doesn't yet recognize.

**Packaging / deployment:**
- **[open]** Deployment recipes (listening room, home theater, small venue, audiophile rack).
- **[open]** Debian packages for talker and Linux receiver; firmware image for MCU receiver.
- **[open]** Project website with docs and examples.

**Time estimate:** 4–6 weekends across tracks plus calendar time for soak runs. Wire-format / talker / receiver pieces land in individual PRs as they're ready; hardware-dependent validation is gated on physical access.

### M9 — Ravenna / AES67 interop

**Goal:** First-class interop with the Ravenna / AES67 ecosystem via Mode 4 (RTP over UDP). Opens the project to a much larger adjacent market: any AES67 device (Merging Anubis, Neumann MT 48, any Ravenna-native gear, any Dante-with-AES67-mode device, any `aes67-linux-daemon` deployment) becomes a valid talker or listener alongside AOEther-native endpoints.

Ships in phases:

**Phase A — [shipped] RTP/AES67 wire encoding.** `--transport rtp` on both talker and receiver, 12-byte RTP header (RFC 3550) followed by L24 big-endian PCM payload (RFC 3190 / AES67 §7). PTIME is 1 ms by default (AES67 standard) and 125 µs optionally (the AES67 low-latency profile, matching AOEther's native microframe cadence). Destination typically IPv4 multicast `239.X.X.X` on UDP port 5004. Mode C feedback is disabled under RTP — AES67 devices expect PTPv2 for clocking and will discard our 0x88B6 frames. Fragmentation is disabled (strict AES67 listeners won't reassemble); configs that exceed MTU are rejected at startup. Only the default dynamic payload type (96 for L24) is emitted/accepted today; SDP-negotiated PTs arrive in Phase B.

**Phase B — [shipped] SDP / SAP discovery.** SDP session description per AES67 conventions; SAP (RFC 2974) announcement every 30 s, with a deletion packet on graceful shutdown. Both address families are supported symmetrically: an IPv4 `--dest-ip` announces on `239.255.255.255:9875` with an `IN IP4` SDP, an IPv6 `--dest-ip` announces on `[ff0e::2:7ffe]:9875` with `A=1` in the SAP header and an `IN IP6` SDP. User-facing: `--announce-sap` and `--session-name` on the talker; `--sdp-only` prints the SDP to stdout for controllers that want a static file. Receiver gets `--list-sap [SECS]` — passive sniffer that listens on both SAP families, dedupes sessions by `(family, origin, msg-id)`, prints every unique session seen plus a ready-to-run receiver command, then exits. Auto-bind-on-discovery is [open] pending a design decision on format-mismatch behavior.

**Phase C — [shipped, talker] PTPv2 default-profile integration.** PTPv2 default profile is separate from gPTP (802.1AS) that Milan uses; `ptp4l` supports both. `--ptp` on the talker switches the RTP timestamp base from `CLOCK_MONOTONIC` to `CLOCK_TAI` (which `phc2sys` slews against the PTP grandmaster) and adds `a=ts-refclk:ptp=IEEE1588-2008:<gmid>:<domain>` + `a=mediaclk:direct=0` to the emitted SDP — the grandmaster identity comes from linuxptp's `pmc` tool, re-read every 30 s and propagated into SDP via a `session_version` bump on BMCA change. When `pmc` isn't available or no master is elected, the talker falls back to `a=ts-refclk:ptp=IEEE1588-2008:traceable`. `--ptp-domain N` selects the advertised domain (default 0). The `ptp4l` config recipe lives in `docs/ptp-setup.md`. Full hardware validation (sub-µs accuracy, soak testing) is part of Phase D once Tier 2 boards ship.

**Phase D — [hardware-blocked] Interop validation.** Confirmed playback with `aes67-linux-daemon` (open-source reference), Merging ANEMAN / Anubis, a Dante-with-AES67-mode device, and a Neumann or similar Ravenna endpoint.

**Documented constraints:**
- AES67 is PCM only, so DSD streams remain on Modes 1 / 3 (with AoE header). The talker / receiver reject `--transport rtp --format dsd*` at startup.
- AES67 channel counts typically cap at 8 per stream; higher counts split across multiple substreams via RFC 5888 `a=group:LS` SDP bundling (M10).
- L16 payload is wire-format-ready (`rtp_swap16_inplace` exists) but not yet plumbed as a `--format` option — s24 is AOEther's PCM lock.

**Key risks:**
- AES67 has enough ambiguity in practice that interop edge cases are expected. Approach: start with `aes67-linux-daemon` as a known-good reference, then expand to commercial gear with iteration.
- SAP multicast is sometimes blocked by consumer routers; document workarounds (static SDP files, manual stream addition).
- PTP domain coexistence with gPTP deployments (Milan uses domain 0 typically; AES67 setups often use other domains). Ensure AOEther can participate in both simultaneously if needed.

**Time estimate:** 4–6 weekends total; Phase A is the first weekend's worth.

### M10 — Multi-stream sample-aligned AES67 emission

**Goal:** Support channel counts above the AES67 per-stream cap of 8 by splitting an N-channel source into ⌈N/8⌉ coordinated AES67 substreams whose RTP timestamps are PTP-aligned and whose relationship is declared to listeners via RFC 5888 `a=group:LS` SDP bundling. The concrete user story is a 7.1.6 (14 ch) or 9.1.6 (16 ch) immersive playback rig where the content is already channel-based PCM — Atmos pre-rendered to FLAC at rip time, or native channel-based releases — the network is PTPv2 end-to-end with a Mellanox Spectrum switch as the grandmaster, and the listeners are either AES67-native powered speakers (Genelec SAM, Dutch & Dutch 8c, Neumann KH series — **Path A**) or a consolidated pro AES67-to-analog bridge driving conventional speakers (Merging Hapi MKII, Focusrite RedNet — **Path B**). AOEther's role is to be a clean PTP-disciplined multichannel source; room correction, bass management, and any Atmos object rendering remain upstream concerns handled by HQPlayer, CamillaDSP, or a Dolby Atmos Renderer run offline.

Ships in phases:

**Phase A — [shipped] Multi-stream wire encoding.** Talker splits its N-channel ALSA input into M substreams of at most 8 channels each (configurable via `--channels-per-stream`, default 8, first-fit so 14 ch → 8 + 6). Each substream gets its own UDP destination port and, by default, its own multicast group (`239.X.X.10`, `.11`, …, deriving from `--dest-ip`), so listeners can join only the substreams whose channels they reproduce. Explicit per-substream addressing is also accepted via a repeatable `--stream <id>:<ch-range>:<dest-ip>:<port>` flag for operator-managed layouts (unicast, IPv6, non-auto-derivable orderings); IPv6 destinations are bracketed (`[ff0e::101]`) so inner colons don't confuse the port separator. All substreams share the same RTP timestamp domain: a given sample index N has the same RTP timestamp in every substream, so a listener that reassembles substreams into one output gets sample-accurate reconstruction. SSRCs and sequence numbers are independent per substream; PTIME and payload type match across substreams (enforced at startup). Channel order within a substream is the ALSA input order unchanged — upstream (HQPlayer / CamillaDSP / player) is responsible for presenting channels in SMPTE / Dolby / ITU-R BS.2051 order when the listener expects a specific convention.

**Phase B — [shipped] SDP bundling and SAP announce.** One SDP per session containing multiple `m=audio` sections, one per substream, each tagged `a=mid:<id>`. Session-level `a=group:LS <mid1> <mid2> …` declares lip-sync grouping per RFC 5888 §5.2. `o=`, `s=`, `t=` shared across media sections; per-m `c=` for per-substream multicast groups. `a=ts-refclk:ptp=…` and `a=mediaclk:direct=0` are lifted to session level for the bundled form so all media inherit the same media clock; `--ptp` and the BMCA-derived grandmaster identity continue to apply. SAP announces the bundled SDP as a single session (one SAP message carries the full multi-m= SDP); `session_version` bumps on BMCA change or on any substream reconfiguration. `--sdp-only` prints the full bundled SDP to stdout for static-SDP consumers. Single-stream output (n=1) continues to use the M9 `sdp_build` path with the session-level `c=` and media-level `a=ts-refclk` placement, preserving v1.5 wire-format behavior bit-for-bit when no multi-stream split is in effect.

**Phase C — [partial] Receiver-side multi-stream join.** `sdp_parse_bundle` in `common/sdp.c` parses multi-m= SDPs into session + per-media descriptors; `receiver --sdp <file>` reads a PATH, parses, and prints the substream layout. For single-m= SDPs the parsed fields are applied as implicit `--transport rtp` + `--group` + `--port` + `--channels` + `--rate` so controller-issued SDP files drop straight into the existing single-stream receive path. Multi-m= SDPs print the layout and exit cleanly — the actual multi-socket reassembler (N sockets joined to per-substream groups, poll across them, per-timestamp reassembly into one ALSA output whose channel count is the sum across substreams, cross-substream skew watchdog logging beyond 1 ms) lands alongside Phase D hardware interop validation, since the receive loop wants to be shaken out against real Genelec / D&D / Hapi traffic rather than against synthetic loopback. `--sap-select <session-id>` is deferred to the same landing.

**Phase D — [hardware-blocked] Interop validation.** Path A: Genelec SAM with Dante-AES67 module; Dutch & Dutch 8c across the full speaker array. Path B: Merging Hapi MKII in 8 ch + 6 ch configuration. Reference: `aes67-linux-daemon` as a software listener, validating multi-m SDP and RTP against an open-source implementation before engaging commercial gear. Per-manufacturer quirks documented in `docs/recipe-multichannel-aes67.md`.

**Documented constraints:**
- M10 applies only to Mode 4. Modes 1 / 3 carry arbitrary channel counts in a single AoE frame group via M8 packet splitting; no multi-stream primitive is needed there and none is added.
- DSD at > 2 channels across distributed endpoints is not covered — architecturally incompatible with a PTP-master clock domain across multiple free-running DSD DACs. Stereo DSD remains on Modes 1 / 3 with a single DAC.
- Listeners that ignore `a=group:LS` can still bind substreams individually and will play correctly provided they join the right multicast groups, but will not assert lip-sync-group semantics. Phase D's first task is characterising which listeners parse the group line vs. ignore it.
- Atmos object rendering remains out of scope — content must arrive at the talker already as channel-based PCM. Rendering from object sources (MKV rips, streamed Atmos) happens upstream and offline, typically via a Dolby Atmos Renderer on macOS / Windows at rip time.
- Bass management and room correction are upstream DSP concerns (HQPlayer, CamillaDSP) and not added to AOEther.

**Key risks:**
- Multi-m SDP and `a=group:LS` support is uneven across AES67 listeners; `aes67-linux-daemon` is the canary before commercial gear.
- Multicast group allocation colliding with operator addressing — the explicit `--stream` form is the escape hatch; the chosen set of (group, port, channel-range) tuples is logged at startup for audit.
- Switch PTP configuration: with Mellanox Spectrum as grandmaster the switch BC must run the IEEE 1588-2008 default profile (domain 0), not gPTP / 802.1AS; document in `ptp-setup.md`.
- Receiver-side jitter-buffer sharing across substreams: one buffer, one fill level, keyed off the common RTP timestamp. A substream falling far behind the bundle must not stall the others; the cross-stream skew watchdog exists to surface this rather than hide it.

**Time estimate:** 2–3 weekends implementation + 1 weekend Phase D once hardware lands. Wire format unchanged (RTP header is not touched); `wire-format.md` gets a new subsection on multi-stream SDP bundling. New recipe `docs/recipe-multichannel-aes67.md` covers the 7.1.6 Path A and Path B walkthroughs end-to-end.

### M11 — Path C: PHC-slaved audio clock reference endpoint

**Goal:** A documented reference design for a DIY AES67 endpoint whose audio output clock is physically slaved to the PTP hardware clock, targeting builders who want the Path C architecture (from M10) without buying Merging-class hardware. Framed as a community-driven project under AOEther's umbrella rather than a required milestone: AOEther supplies the software path (receiver code already handles this via M9 + M10), M11 supplies the hardware reference and driver-integration notes.

**Why Path C and not plain ClearFog + USB DAC:** A Marvell ClearFog CN913x with `ptp4l` + `phc2sys` running AOEther's M9 RTP listener over a USB DAC is trivially buildable and supported on main today — but the USB DAC's internal crystal is free-running, so network-side PTP alignment does not translate to audio-clock alignment at the DAC. Across N such endpoints the clock-domain story reverts to the free-running-crystal problem that distributed multichannel otherwise avoids. Path C is the engineering project of making the endpoint's audio output clock actually derive from the PHC — which is what Merging, Neumann, Genelec, and Dutch & Dutch have all engineered into their own boxes, and what keeps their arrays sample-coherent.

**Scope (expand as contributors deliver):** Reference schematic and BOM for a ClearFog CN913x carrier with I2S output to a well-regarded DAC chip (ES9038Q2M or AK4493-class), MCLK derived from the PHC via a fractional-N PLL driven from a GPIO or dedicated PHC-out pin. Kernel and device-tree notes for exposing the PHC-derived MCLK to the ALSA SoC driver so that sample-rate changes reconfigure the PLL coherently. Optional variant: AES3 output from a transmitter IC whose bit clock is the PHC-derived MCLK, feeding a powered speaker that AES3-slaves its internal DAC. Validation: long-soak drift comparison between a Path-C endpoint and a Genelec SAM across a multi-day session, quantifying that the DIY path achieves parity within the PTP sync budget.

**Out of scope for M11 (explicit):** Productization, PCB fabrication service, or warranty. Non-ClearFog platforms — contributors can port, but the reference stays singular. USB-DAC-based endpoints — a plain USB DAC reintroduces a free-running crystal and does not satisfy Path C by definition.

**Status:** [open, community-driven] indefinitely. AOEther core maintenance does not depend on M11 closing; M10 is sufficient for Paths A and B, which cover the realistic commercial-hardware deployment.

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

**Why GPL-3.0-or-later?** Earlier drafts of this document proposed Apache 2.0 with the argument that it would attract pro-AV vendor contributions that GPL would block. That calculus shifted once the Milan control-plane story anchored on `la_avdecc` (L-Acoustics' AVDECC reference implementation), which is itself LGPL-3 / GPL-3 dual-licensed — the AOEther receiver that speaks AVDECC links against la_avdecc, so AOEther being GPL-3 aligns cleanly with the ecosystem we're actually interoperating with instead of creating license friction at every Milan integration point. GPL-3's patent grant (Section 11) addresses the TSN-patent concern that originally motivated Apache's explicit grant, and "or-later" keeps the door open to GPL-4+ when it ships. The trade-off is accepted: downstream users who want to ship AOEther inside a closed-source product can't — they must upstream their changes or pick different software. That's the right direction for a project whose value-add is open clock-discipline and open wire-format; closed-source forks of the clock-discipline code would erode exactly the interop story the project exists to advance.

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
