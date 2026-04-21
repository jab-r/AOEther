# AOEther Wire Format Specification

**Status:** v1.1 draft — aligned with [`design.md`](design.md) v1.1
**Purpose:** Byte-level reference for implementers of talkers, receivers, and test tools.

This document specifies the AOEther wire format at the level of detail needed to build an interoperable implementation. For architectural rationale, see [`design.md`](design.md).

## Transport encapsulation

The 16-byte AoE header and its audio payload are identical across all transport modes. Only the outer wrapper varies.

```
Mode 1 (L2, M1–M3 default):
┌──────────────────┬──────────────────┬──────────┬───────────┐
│ Ethernet header  │ AoE header       │ Audio    │ Padding & │
│ (14 bytes)       │ (16 bytes)       │ payload  │ FCS       │
└──────────────────┴──────────────────┴──────────┴───────────┘
  EtherType = 0x88B5

Mode 2 (AVTP, M5+):
┌──────────────────┬────────────────────────┬──────────┬─────┐
│ Ethernet header  │ AVTP header + AAF      │ Audio    │ FCS │
│ (14 bytes)       │ or vendor-DSD subtype  │ payload  │     │
└──────────────────┴────────────────────────┴──────────┴─────┘
  EtherType = 0x22F0

Mode 3 (IP/UDP, M4+):
┌──────────────────┬──────────┬──────┬──────────────────┬──────────┬─────┐
│ Ethernet header  │ IPv4/v6  │ UDP  │ AoE header       │ Audio    │ FCS │
│ (14 bytes)       │ (20/40B) │ (8B) │ (16 bytes)       │ payload  │     │
└──────────────────┴──────────┴──────┴──────────────────┴──────────┴─────┘
  UDP port = 8805 (interim, pending IANA)

Mode 4 (RTP/AES67, M8 stretch, PCM only):
┌──────────────────┬──────────┬──────┬──────────────────┬──────────┬─────┐
│ Ethernet header  │ IPv4/v6  │ UDP  │ RTP (AES67)      │ PCM      │ FCS │
│ (14 bytes)       │          │      │ (12 bytes)       │ payload  │     │
└──────────────────┴──────────┴──────┴──────────────────┴──────────┴─────┘
  No AoE header; RTP payload type and format per AES67 profile.
```

Modes 1–3 carry the same AoE header and any supported format code (PCM, DoP, native DSD). Mode 4 is a separate PCM-only encoding for AES67 interop; see AES67 / RFC 3190 for RTP payload details. This document covers the AoE header itself, which appears in Modes 1, 2 (DSD vendor subtype), and 3.

**Destination address:**
- Mode 1 / Mode 2: Ethernet unicast for point-to-point; multicast MAC range `01:1B:19:00:00:00/40` (AVTP reserved) for future multicast streams.
- Mode 3: IPv4 unicast, IPv4 multicast (`239.0.0.0/8`), IPv6 unicast, or IPv6 multicast (`ff0e::/16`).
- Mode 4: per AES67 — IPv4 multicast `239.0.0.0/8` typical.

**QoS marking:** Mode 3 talkers set DSCP `EF` (Expedited Forwarding, 0x2E) on egress packets to encourage WMM AC_VO prioritization on WiFi and priority handling on DSCP-aware wired switches.

**Frame size:** Standard 1500-byte MTU. Minimum Ethernet frame (64 bytes including header and FCS) is satisfied by padding when payload is small. Jumbo frames are not used in any AOEther configuration.

## AoE header (16 bytes)

All multi-byte integer fields are big-endian.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    Magic      |    Version    |          Stream ID            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       Sequence Number                         |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      Presentation Time                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
| Channel Count |    Format     | Payload Count |    Flags      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### Field reference

| Offset | Field | Size | Type | Notes |
|--------|-------|------|------|-------|
| 0 | Magic | 1 | `u8` | Constant `0xA0`. Receivers MUST discard frames with any other value. |
| 1 | Version | 1 | `u8` | Protocol version. Currently `0x01`. Receivers SHOULD discard frames with unrecognized versions. |
| 2 | Stream ID | 2 | `u16` BE | Identifies the logical audio stream. Allocated by the talker at stream setup. |
| 4 | Sequence Number | 4 | `u32` BE | Monotonically increasing per stream, wraps at 2^32. Receivers use this to detect loss and reorder if needed. |
| 8 | Presentation Time | 4 | `u32` BE | Low 32 bits of the gPTP timestamp (nanoseconds) at which the first sample in this packet should be presented. Zero when no PTP is available (M1–M2). |
| 12 | Channel Count | 1 | `u8` | Number of audio channels, 1 to 64. |
| 13 | Format | 1 | `u8` | Format code; see table below. |
| 14 | Payload Count | 1 | `u8` | Format-dependent: samples per channel for PCM/DoP; DSD bytes per channel for native DSD. |
| 15 | Flags | 1 | `u8` | Bitmask; see below. |

### Format codes

| Code | Format | Payload semantics |
|------|--------|-------------------|
| `0x00` | Reserved | Must not appear on the wire |
| `0x10` | PCM s16le | Signed 16-bit little-endian, 2 bytes/sample |
| `0x11` | PCM s24le-3 | Signed 24-bit little-endian, packed into 3 bytes/sample |
| `0x12` | PCM s24le-4 | Signed 24-bit little-endian, padded in 4 bytes/sample (high byte = 0) |
| `0x13` | PCM s32le | Signed 32-bit little-endian, 4 bytes/sample |
| `0x20` | DoP-DSD64 | PCM s24le-3 at 176.4 kHz carrying DSD bits with markers |
| `0x21` | DoP-DSD128 | PCM s24le-3 at 352.8 kHz |
| `0x22` | DoP-DSD256 | PCM s24le-3 at 705.6 kHz |
| `0x23` | DoP-DSD512 | PCM s24le-3 at 1411.2 kHz (requires host/DAC support for this rate) |
| `0x30` | Native DSD64 | Raw DSD bitstream at 2.8224 MHz/channel, MSB-first |
| `0x31` | Native DSD128 | Raw DSD at 5.6448 MHz/channel |
| `0x32` | Native DSD256 | Raw DSD at 11.2896 MHz/channel |
| `0x33` | Native DSD512 | Raw DSD at 22.5792 MHz/channel |
| `0x34` | Native DSD1024 | Raw DSD at 45.1584 MHz/channel |
| `0x35` | Native DSD2048 | Raw DSD at 90.3168 MHz/channel |

Unassigned format codes are reserved for future use. Receivers MUST discard frames with unrecognized format codes (and MAY log a diagnostic).

### Flags byte

```
Bit 0 (LSB): last-in-group
Bit 1:       discontinuity
Bit 2:       marker
Bits 3-7:    reserved (must be 0 on transmit, ignored on receive)
```

- **last-in-group (bit 0):** Set on the final packet of a microframe when a single microframe's audio is split across multiple packets (e.g., DSD2048 stereo where 2822 bytes per microframe exceeds MTU). When split is not in use, this bit is always 1.
- **discontinuity (bit 1):** Set by the talker when it knows there's a gap in continuity (e.g., after a talker restart or source reconnect). Receivers use this to reset jitter buffer state without reporting an underrun.
- **marker (bit 2):** Application-defined. May be used in future for stream-level metadata (e.g., stream start, loudness event). Receivers pass through.

## Payload encoding

Payload immediately follows the 16-byte header with no alignment padding.

### PCM and DoP

Samples are interleaved by channel, little-endian, signed. Total payload length in bytes:

```
length = channel_count × bytes_per_sample × payload_count
```

where `bytes_per_sample` is 2 (`s16le`), 3 (`s24le-3`), or 4 (`s24le-4`, `s32le`) per format code.

**Channel ordering:** Standard ITU-R BS.2051 / SMPTE conventions for common layouts:

- Stereo: L, R
- 5.1: L, R, C, LFE, Ls, Rs
- 7.1: L, R, C, LFE, Ls, Rs, Lss, Rss
- 7.1.4: L, R, C, LFE, Ls, Rs, Lss, Rss, Ltf, Rtf, Ltr, Rtr
- 22.2: per SMPTE ST 2036-2 channel numbering

For configurations outside these standards, a channel map is advertised via the control plane and carried out-of-band from the audio wire format.

**DoP encoding** uses the `PCM s24le-3` layout with alternating marker bytes (`0x05` / `0xFA`) in the high byte of each 24-bit sample and DSD bits in the low 16 bits. The receiver's DAC detects DoP by the marker byte pattern and switches to DSD playback automatically.

### Native DSD

Payload is the raw DSD bitstream, interleaved by channel, MSB-first within each byte. Total payload length:

```
length = channel_count × payload_count
```

where `payload_count` is the number of DSD bytes per channel in this packet (each byte carries 8 DSD bits).

DSD rate determines average bytes per microframe per channel:

| Format | Bytes/microframe/channel (avg) |
|--------|-------------------------------:|
| Native DSD64 | 44.1 |
| Native DSD128 | 88.2 |
| Native DSD256 | 176.4 |
| Native DSD512 | 352.8 |
| Native DSD1024 | 705.6 |
| Native DSD2048 | 1411.2 |

Because these are non-integer, the talker alternates packet sizes (e.g., 44 and 45 bytes) to achieve the average rate over time. The exact alternation pattern is implementation-defined; receivers MUST accept any legal `payload_count` ≥ 1.

## Cadence

Packets are emitted at a nominal rate of 8000 packets per second per stream, matching the USB High-Speed microframe rate. Each packet is timed to correspond to one USB microframe on the receiver's USB output path.

For payload sizes that exceed the MTU (native DSD2048 stereo is 2822 bytes per microframe, exceeding the standard 1500-byte MTU), the talker splits a single microframe's audio across multiple packets transmitted in immediate succession. The last packet in the group sets the `last-in-group` flag. All packets within a group share the same sequence number and presentation time; the receiver reassembles them into a single contiguous audio unit for the USB OUT endpoint.

**Example:** DSD2048 stereo microframe = 2 × 1411.2 = ~2822 bytes per microframe. Split into two packets of ~1411 bytes each (payloads, plus 16-byte headers), both with the same sequence number. First packet has `last-in-group` cleared; second has it set.

## Frame size examples

| Configuration | Payload length | Frame length (incl. 14B Eth + 16B AoE + 4B FCS) |
|---------------|---------------:|------------------------------------------------:|
| Stereo PCM 48 kHz/24 | 36 B | 70 B (padded to 64-byte minimum) |
| 5.1 PCM 48 kHz/24 | 108 B | 142 B |
| 7.1 PCM 48 kHz/24 | 144 B | 178 B |
| 7.1.4 PCM 48 kHz/24 | 216 B | 250 B |
| 22.2 PCM 48 kHz/24 | 432 B | 466 B |
| 32ch PCM 48 kHz/24 | 576 B | 610 B |
| 32ch PCM 96 kHz/24 | 1152 B | 1186 B |
| Stereo Native DSD64 | 88 B | 122 B |
| Stereo Native DSD512 | 704 B | 738 B |
| Stereo Native DSD2048 | 2822 B total → 2 pkts of 1411 B | 2 × 1445 B |

## Stream lifetime

Streams are created via the control plane (out of scope for this document; see [`design.md`](design.md) M6+ for AVDECC / mDNS-SD discovery). At the wire-format level:

- The first packet of a stream SHOULD have sequence number `0`, but receivers MUST accept any starting sequence number.
- Receivers track the highest sequence number seen per stream and detect loss via gaps.
- A talker MAY set the `discontinuity` flag on the first packet after any interruption.
- Stream teardown is out-of-band (control plane); in the absence of a teardown signal, receivers SHOULD treat silence for more than 500 ms as a stream stall and reset jitter buffer state on the next packet with `discontinuity` set.

## Forward compatibility

Implementations SHOULD:

- Reject any frame with Magic ≠ `0xA0`.
- Reject any frame with Version ≠ `0x01` (until a versioning negotiation protocol is defined).
- Reject any frame with an unrecognized Format code.
- Ignore reserved bits in Flags.

Future versions may redefine reserved fields; implementations MUST NOT assume reserved values are always zero on the wire.

## Diagnostic header example

```
Bytes (hex, space-separated for clarity):
A0 01 00 01 00 00 00 2A 00 00 00 00 02 11 06 01
│  │  │     │           │           │  │  │  │
│  │  │     │           │           │  │  │  └─ flags: last-in-group
│  │  │     │           │           │  │  └──── payload count: 6 samples/ch
│  │  │     │           │           │  └─────── format: 0x11 (PCM s24le-3)
│  │  │     │           │           └────────── channels: 2
│  │  │     │           └────────────────────── presentation time: 0
│  │  │     └────────────────────────────────── sequence: 42
│  │  └──────────────────────────────────────── stream ID: 1
│  └─────────────────────────────────────────── version: 1
└────────────────────────────────────────────── magic: 0xA0
```

Followed by 36 bytes of PCM payload (2 channels × 3 bytes × 6 samples).
