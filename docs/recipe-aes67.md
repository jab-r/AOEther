## Recipe: AOEther as an AES67 talker / listener (M9 Phase A)

Mode 4 carries audio as plain RTP with an L24 payload, matching the AES67 / Ravenna data plane. This lets AOEther endpoints interoperate with aes67-linux-daemon, Merging Anubis, Neumann MT 48, Dante-with-AES67, and other AES67-compliant devices on the same network.

This recipe covers **Phase A only** — RTP wire encoding is live; SDP session description, SAP discovery, and PTPv2 timestamp discipline are tracked separately (see `design.md` M9 phases B / C).

For the byte layout see [`wire-format.md`](wire-format.md) §"Mode 4 (RTP / AES67)".

## Talker → AES67 listener

Emit a stereo 48 kHz L24 stream at AES67's default PTIME (1 ms):

```sh
sudo ./build/talker --iface eno1 \
                    --transport rtp \
                    --dest-ip 239.69.1.10 \
                    --port 5004 \
                    --source testtone \
                    --channels 2 --rate 48000
```

Switch to the AES67 low-latency profile (125 µs PTIME, matching AOEther's native cadence):

```sh
sudo ./build/talker --iface eno1 --transport rtp \
                    --dest-ip 239.69.1.10 --ptime 125 \
                    --source testtone --channels 2 --rate 48000
```

What the talker does in Mode 4:

- Opens a UDP socket; joins no group (multicast-send doesn't require membership).
- DSCP EF marking on egress, same as Mode 3.
- Builds an RTP header per packet: `V=2`, `P=0`, `X=0`, `CC=0`, `M=0`, `PT=96` (dynamic default for L24), monotonic 16-bit sequence, timestamp advancing by `payload_count` samples per packet.
- Byte-swaps each 24-bit sample from ALSA's little-endian to the wire's big-endian.
- Does not emit Mode C feedback frames — AES67 devices expect PTPv2 for clocking and would drop our 0x88B6 frames anyway.
- Does not start an AVDECC entity even if `--avdecc` is passed — AVDECC is a Milan concept; AES67 uses SAP / SDP.

## AOEther receiver ← AES67 talker

The same endpoint can also listen to an AES67 stream from any compliant talker:

```sh
sudo ./build/receiver --iface eth0 --dac hw:CARD=D90,DEV=0 \
                      --transport rtp \
                      --group 239.69.1.10 \
                      --port 5004 \
                      --channels 2 --rate 48000
```

`--group` joins the IGMP multicast group; omit it for unicast deployments where the sender targets the receiver's own IP. The receiver parses RTP, byte-swaps L24 samples to little-endian, and writes to ALSA via the same path used by Modes 1/3. Mode C feedback is not emitted (see above).

## Interop test — aes67-linux-daemon

The open-source reference is [aes67-linux-daemon](https://github.com/bondagit/aes67-linux-daemon). Install it on a second Linux box on the same segment, create a sink matching AOEther's stream (239.69.1.10:5004, 48 kHz, stereo, L24, PTIME=1 ms, PT=96), and it should show up with live packets flowing. Clocking drift will be visible — see "Clock considerations" below.

Dante-with-AES67-mode, Merging, and Neumann devices expect SDP / SAP for stream description; they won't auto-discover a raw RTP stream from this Phase A talker. Until Phase B ships SAP, feed them a static SDP file manually.

An AES67-compliant static SDP for the talker example above looks like:

```
v=0
o=- 0 0 IN IP4 192.168.1.100
s=AOEther test stream
c=IN IP4 239.69.1.10/32
t=0 0
m=audio 5004 RTP/AVP 96
a=rtpmap:96 L24/48000/2
a=ptime:1
a=recvonly
a=source-filter: incl IN IP4 * 192.168.1.100
```

Substitute your talker's host IP for `192.168.1.100`. For the low-latency profile change `a=ptime:1` to `a=ptime:0.125`. Most AES67 devices will accept the stream once pointed at this SDP.

## Clock considerations

**There is no PTP in this phase.** The talker emits on a local monotonic timer; the RTP timestamp advances by `payload_count` samples per packet, measured from a local start time. AES67 listeners typically expect the timestamp to be PTP-disciplined — they will play the stream, but clock drift relative to the listener's PTP grandmaster will cause buffer-level drift over long sessions.

For short sessions (a few minutes), drift is small and usually not audible. For continuous playback, PTPv2 integration is required — tracked as M9 Phase C.

If the listener exposes a "free-run" or "no-lock" mode, enable it when testing against this Phase A talker.

## Known limitations (Phase A)

- No SDP, no SAP. Stream metadata must be conveyed out-of-band.
- No PTPv2 timestamp locking.
- No payload-type negotiation — talker emits PT=96 unconditionally, receiver accepts any PT.
- L16 encoding path exists in the common module but isn't exposed yet.
- 44.1 kHz / 88.2 kHz / 176.4 kHz work at the wire level but AES67 deployments overwhelmingly use 48 kHz / 96 kHz.
- `--transport rtp --format dsd*` is rejected; AES67 is PCM-only. DSD streams remain on Modes 1 / 3.
- Multichannel beyond stereo works but AES67 conventions cap per-stream channel count at 8. Higher counts split across streams at the SDP level (Phase B).
