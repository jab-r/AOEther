# AOEther Wire Format Specification

**Status:** v1.5 draft — aligned with [`design.md`](design.md) v1.5
**Purpose:** Byte-level reference for implementers of talkers, receivers, and test tools.

This document specifies the AOEther wire format at the level of detail needed to build an interoperable implementation. For architectural rationale, see [`design.md`](design.md).

AOEther has two wire protocols that coexist on the same interface:

- **Data frames** (EtherType `0x88B5`, §"AoE header") carry audio samples from talker to receiver.
- **Control frames** (EtherType `0x88B6`, §"Control frames") carry small messages between endpoints, currently only UAC2-shape clock-discipline feedback from receiver to talker. This frame class is new in v1.3 and supports Mode C clock discipline (see `design.md` §"Clock architecture").

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

Mode 4 (RTP/AES67, M9, PCM only):
┌──────────────────┬──────────┬──────┬──────────────────┬──────────┬─────┐
│ Ethernet header  │ IPv4/v6  │ UDP  │ RTP (AES67)      │ PCM      │ FCS │
│ (14 bytes)       │          │      │ (12 bytes)       │ payload  │     │
└──────────────────┴──────────┴──────┴──────────────────┴──────────┴─────┘
  No AoE header; RTP payload type and format per AES67 profile.
```

Modes 1, 3, and the DSD vendor-subtype path of Mode 2 carry the same AoE header and any supported format code (PCM, DoP, native DSD). The PCM path of Mode 2 carries IEEE 1722 AVTP AAF instead — see §"Mode 2 (AVTP AAF)" below. Mode 4 is a separate PCM-only encoding for AES67 interop; see AES67 / RFC 3190 for RTP payload details. This document covers the AoE header (§"AoE header"), the AAF header (§"Mode 2 (AVTP AAF)"), and the shared AoE-C control header (§"Control frames").

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

Payload is the raw DSD bitstream, interleaved by channel **at byte granularity**, MSB-first within each byte. For a 2-channel DSD stream with `payload_count = N` the payload is:

```
L[0] R[0] L[1] R[1] ... L[N-1] R[N-1]
```

For `C` channels and `N` bytes per channel the payload is `C × N` bytes total, with per-byte round-robin interleave across channels. This matches `SND_PCM_FORMAT_DSD_U8` exactly, so a receiver using that ALSA format can pass the payload to `snd_pcm_writei` with no reorder. Receivers using `SND_PCM_FORMAT_DSD_U16_*` or `DSD_U32_*` MUST transpose at the corresponding granularity (2 or 4 bytes per channel) before writing.

Bit order within each byte is MSB-first: the high bit of the byte is the older DSD sample, the low bit is the newer. This is opposite of the Sony DSF file format (which stores DSD LSB-first) — implementations ingesting DSF files must bit-reverse each byte on read.

Total payload length:

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

## Mode 2 (AVTP AAF)

When the talker is configured with `--transport avtp`, PCM streams are emitted as IEEE 1722-2016 AVTP AAF frames on EtherType `0x22F0` instead of the AOE wrapper. This is what makes a stream visible to off-the-shelf Milan listeners (Hive-controlled MOTU AVB interfaces, L-Acoustics speakers, audiophile devices that speak Milan, etc.). The control plane (AVDECC entity model, stream reservation) is **not** implemented in M5 — the talker emits frames as if AVDECC had already negotiated the stream, and the listener must be told out-of-band which stream ID and listener-side talker MAC to subscribe to. AVDECC integration arrives in M7.

DSD streams continue to use Mode 1 / Mode 3; AAF doesn't carry DSD.

### AVTP-AAF header (24 bytes)

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   subtype     |sv|version|mr|rsv|tv|  sequence_num |   rsv  |tu|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                          stream_id                            |
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       avtp_timestamp                          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|   format    | nsr  | rsv |     channels_per_frame  | bit_depth|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|       stream_data_length      |sp|         reserved           |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Offset | Field | Width | Notes |
|---|---|---|---|
| 0 | subtype | 8 | `0x02` (AAF) |
| 8 | sv | 1 | `1` (stream_id valid) |
| 9 | version | 3 | `0` (AVTPv0) |
| 12 | mr | 1 | media reset; toggled on stream restart, `0` otherwise |
| 15 | tv | 1 | `1` if `avtp_timestamp` is valid; `0` until M3 PTP lands |
| 16 | sequence_num | 8 | per-stream packet counter, wraps at 256 |
| 31 | tu | 1 | timestamp uncertain; `0` for valid, `1` while gPTP is unlocked |
| 32 | stream_id | 64 | AOEther mints `(src_mac << 16) | stream_id` until M7 |
| 96 | avtp_timestamp | 32 | gPTP nanoseconds, low 32 bits; `0` when `tv=0` |
| 128 | format | 8 | `0x03` (INT_24) for our s24 PCM; `0x04` INT_16, `0x02` INT_32, `0x01` FLOAT_32 also defined by spec |
| 136 | nsr | 4 | sample-rate code (Table below) |
| 144 | channels_per_frame | 10 | 1..1023 |
| 152 | bit_depth | 8 | bits actually used per sample (24 for our INT_24) |
| 160 | stream_data_length | 16 | bytes of PCM payload following the header |
| 176 | sp | 1 | sparse mode; `0` for normal |

Bit packing follows IEEE 1722-2016 Table 18 — the 32-bit `format / nsr / cpf / bit_depth` word at octets 16-19 packs as: format in the high byte, nsr in the next nibble, two reserved bits, then 10 bits of channels_per_frame, then 8 bits of bit_depth. The 32-bit `stream_data_length / sp / reserved` word at octets 20-23 carries the data length in the high half. The `common/avtp.c` helpers `avtp_aaf_hdr_build()` / `avtp_aaf_hdr_parse()` produce and consume this layout.

### NSR codes

| Code | Rate (Hz) |
|---:|---:|
| `0x01` | 8000 |
| `0x02` | 16000 |
| `0x03` | 32000 |
| `0x04` | 44100 |
| `0x05` | 48000 |
| `0x06` | 88200 |
| `0x07` | 96000 |
| `0x08` | 176400 |
| `0x09` | 192000 |
| `0x0A` | 24000 |

### Sample byte order

**AAF samples are big-endian on the wire.** AOEther's source/sink path (ALSA `S24_3LE`) is little-endian, so the talker byte-swaps each 3-byte sample on AVTP egress and the receiver byte-swaps on AVTP ingress. This is the only data-path divergence between Mode 1/3 (which keep ALSA-native LE on the wire) and Mode 2.

### Mode C feedback over AVTP

AOEther's Mode C clock-discipline FEEDBACK frames continue to flow on EtherType `0x88B6` (see §"Control frames") regardless of data transport. A Milan listener that receives an AVTP stream from AOEther will simply ignore the unknown EtherType — it has no effect on AVTP framing or playback. When AOEther is the *receiver* and the talker is a third-party Milan device, FEEDBACK frames are still emitted but the third-party talker will ignore them; in that deployment Mode C is inactive and the AOEther receiver depends on the Milan talker's gPTP-disciplined media clock for stability.

### Stream addressing

Milan typically uses multicast destination MACs in the AVTP-reserved range `91:E0:F0:00:00:00/40`. AOEther's talker accepts any unicast or multicast MAC via `--dest-mac`; it does not register addresses with a switch via MSRP (that's Avnu/Milan controller territory and arrives no earlier than M7 alongside AVDECC).

## Control frames

Control frames carry out-of-band signaling between endpoints. They share the Ethernet wire with data frames but use a distinct EtherType (`0x88B6`) and a distinct header magic byte (`0xA1` vs. `0xA0`), so the two classes are trivially separable by any receiver. Implementations that do not understand a given control frame type MUST discard it without affecting the audio path.

The only control frame type defined in v1.3 is **FEEDBACK** (type `0x01`), which carries clock-discipline feedback from the receiver to the talker. It mirrors USB UAC2 HS async-feedback format directly — a Q16.16 samples-per-1ms value — so receivers with a real UAC2 feedback path can forward the DAC's feedback value with no unit conversion.

### Transport encapsulation

```
Mode 1 (L2, M1+):
┌──────────────────┬──────────────────┬──────────────────────┐
│ Ethernet header  │ AoE-C header     │ Padding & FCS        │
│ (14 bytes)       │ (16 bytes)       │ (to 64-byte min)     │
└──────────────────┴──────────────────┴──────────────────────┘
  EtherType = 0x88B6
```

Mode 3 (IP/UDP, M4) carries AoE-C frames as UDP datagrams on the same port as data frames (interim 8805), disambiguated by the magic byte (`0xA1` for control vs `0xA0` for data). Feedback always flows unicast from each receiver to the talker, regardless of whether data is carried unicast or multicast; in multicast deployments the talker aggregates FEEDBACK across all subscribed receivers and takes the slowest non-stale rate.

Destination: unicast to the peer's MAC (receiver → talker, talker → receiver for future frame types).

### AoE-C header (16 bytes)

All multi-byte integer fields are big-endian.

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|    Magic      |    Version    |  Frame Type   |    Flags      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Stream ID            |           Sequence            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Value (frame-type-dependent)               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                           Reserved                            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

| Offset | Field | Size | Type | Notes |
|--------|-------|------|------|-------|
| 0 | Magic | 1 | `u8` | Constant `0xA1`. Receivers MUST discard control frames with any other value. Distinguishes control from data frames (`0xA0`). |
| 1 | Version | 1 | `u8` | `0x01`. Receivers SHOULD discard frames with unrecognized versions. |
| 2 | Frame Type | 1 | `u8` | `0x01` = FEEDBACK. Other values reserved. Receivers MUST discard unrecognized types without effect on the audio path. |
| 3 | Flags | 1 | `u8` | Reserved; must be `0` on transmit, ignored on receive. |
| 4 | Stream ID | 2 | `u16` BE | Identifies which audio stream this control frame refers to. Matches the Stream ID in the data-frame header. |
| 6 | Sequence | 2 | `u16` BE | Monotonically increasing per (stream, frame-type), wraps at 2^16. Lets the peer drop out-of-order or stale frames. |
| 8 | Value | 4 | `u32` BE | Frame-type-dependent. For FEEDBACK: Q16.16 samples per 1 ms. |
| 12 | Reserved | 4 | `u32` BE | Must be `0` on transmit, ignored on receive. |

### FEEDBACK frame semantics (Frame Type `0x01`)

The receiver observes the DAC's consumption rate — on Linux, via `snd_pcm_status_get_htstamp()` timestamping and `snd_pcm_delay()` tracking; on the MCU tier, via equivalent USB-host instrumentation — and emits this rate as a Q16.16 samples-per-1ms value. The format is identical to the UAC2 HS async feedback-endpoint format (4-byte Q16.16, samples-per-frame).

Nominal reference values:

| Stream rate | Q16.16 samples/ms | Hex |
|---|---:|---|
| 44.1 kHz | 44.100 | `0x002C199A` |
| 48 kHz | 48.000 | `0x00300000` |
| 88.2 kHz | 88.200 | `0x00583333` |
| 96 kHz | 96.000 | `0x00600000` |
| 176.4 kHz | 176.400 | `0x00B06666` |
| 192 kHz | 192.000 | `0x00C00000` |

Update cadence: SHOULD emit at 20–50 Hz. Lower cadence is allowed but widens the buffer-fill band the receiver must tolerate; higher cadence wastes network capacity for no controller-stability benefit.

Talker handling: treat the latest valid FEEDBACK `Value` as the current rate target. Maintain a fractional sample accumulator. Each outgoing data frame's `payload_count` is the integer part of the accumulator; the fractional residual carries. Data-frame cadence is unchanged — **payload size, not cadence, absorbs drift.** Talkers SHOULD clamp the accepted rate to a safety band (e.g., ±1000 ppm of the nominal stream rate) and SHOULD revert to nominal if no FEEDBACK arrives for several seconds.

Startup: the receiver SHOULD emit its first FEEDBACK frame within 100 ms of beginning playback so the talker doesn't run free for long.

Loss tolerance: FEEDBACK is advisory, not authoritative. A single dropped frame is harmless; the talker simply uses the previous value until the next one arrives. Implementations MUST NOT retransmit FEEDBACK frames.

### Frame size

Ethernet header (14 B) + AoE-C header (16 B) = 30 B, which the MAC layer pads to the 60-byte Ethernet minimum before the 4-byte FCS.

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

## Diagnostic control frame example

A FEEDBACK frame advertising the DAC's current rate as 48.0000 samples/ms:

```
Bytes (hex, space-separated for clarity):
A1 01 01 00 00 01 03 E8 00 30 00 00 00 00 00 00
│  │  │  │  │     │     │           │
│  │  │  │  │     │     │           └─ reserved: 0
│  │  │  │  │     │     └───────────── value (Q16.16): 0x00300000 = 48.000 s/ms
│  │  │  │  │     └─────────────────── sequence: 1000
│  │  │  │  └───────────────────────── stream ID: 1
│  │  │  └──────────────────────────── flags: 0
│  │  └─────────────────────────────── frame type: 0x01 FEEDBACK
│  └────────────────────────────────── version: 1
└───────────────────────────────────── magic: 0xA1
```

On the wire: 14-byte Ethernet header + these 16 bytes + 30 bytes of MAC padding + 4-byte FCS = 64 bytes total.
