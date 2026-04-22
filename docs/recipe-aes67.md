## Recipe: AOEther as an AES67 talker / listener (M9 Phases A–C)

Mode 4 carries audio as plain RTP with an L24 payload, matching the AES67 / Ravenna data plane. This lets AOEther endpoints interoperate with aes67-linux-daemon, Merging Anubis, Neumann MT 48, Dante-with-AES67, and other AES67-compliant devices on the same network.

This recipe covers **Phases A–C**:

- **Phase A** — RTP wire encoding.
- **Phase B** — SDP description + SAP announcement (`--announce-sap`, `--sdp-only`).
- **Phase C** — PTPv2-disciplined media-clock timestamps (`--ptp`, via `ptp4l` + `phc2sys`).

Phase D (fully validated interop against commercial gear) is hardware-blocked.

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

## Phase B: SAP announcement + SDP

Pass `--announce-sap` to have the talker periodically multicast an SDP describing this session on `239.255.255.255:9875`:

```sh
sudo ./build/talker --iface eno1 --transport rtp \
                    --dest-ip 239.69.1.10 \
                    --source testtone --channels 2 --rate 48000 \
                    --announce-sap \
                    --session-name "AOEther / Kitchen DAC"
```

Announcements go out every 30 s while the talker runs. When it exits cleanly (SIGINT / SIGTERM), it sends one SAP deletion packet so controllers drop the session promptly instead of waiting out their session timeout.

Once this is running, Dante Controller (in AES67 mode), Merging ANEMAN, and `aes67-linux-daemon` will auto-discover the session and let you bind a listener with a click.

If the receiving controller prefers a static SDP file (some do), use `--sdp-only` to print the exact SDP the talker would emit and redirect it to a file:

```sh
./build/talker --iface eno1 --transport rtp \
               --dest-ip 239.69.1.10 --port 5004 \
               --channels 2 --rate 48000 \
               --session-name "AOEther / Kitchen DAC" \
               --sdp-only > kitchen.sdp
```

Then feed `kitchen.sdp` to the controller. No sockets are opened in this mode.

## Phase C: PTPv2-disciplined timestamps

Pre-req: `linuxptp` — specifically `ptp4l` synchronizing a PTP hardware clock (PHC) to a grandmaster on the LAN, and `phc2sys` slewing the system clock (including `CLOCK_TAI`) to that PHC. See `docs/ptp-setup.md` for a working config.

Once PTP is running and `chrony` (or `phc2sys -s CLOCK_REALTIME -c CLOCK_TAI` or equivalent) is disciplining `CLOCK_TAI`, pass `--ptp` to the talker:

```sh
sudo ./build/talker --iface eno1 --transport rtp \
                    --dest-ip 239.69.1.10 \
                    --source testtone --channels 2 --rate 48000 \
                    --announce-sap --ptp
```

What changes:

- The RTP timestamp base comes from `CLOCK_TAI` instead of `CLOCK_MONOTONIC`. AES67 listeners that align their own RTP timestamps to PTP will now agree with this talker on the media-clock epoch.
- The emitted SDP grows two lines:
  - `a=ts-refclk:ptp=IEEE1588-2008:traceable`
  - `a=mediaclk:direct=0`
  telling controllers the stream is PTP-traceable.

Drift between talker and listener is no longer bounded by the two crystals — it is bounded by the PTP sync accuracy (typically sub-microsecond on hardware-PTP-capable gear, low milliseconds on software PTP over WiFi).

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

With `--announce-sap`, Dante-with-AES67-mode, Merging, and Neumann devices should pick the session up automatically. For gear that prefers a static SDP (or for networks where SAP multicast is blocked at a router), dump the SDP with `--sdp-only` and feed the file directly.

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

**Without `--ptp`** the talker emits on a local monotonic timer; the RTP timestamp advances by `payload_count` samples per packet, measured from a `CLOCK_MONOTONIC` start time. AES67 listeners typically expect the timestamp to be PTP-disciplined — they will play the stream, but clock drift relative to the listener's PTP grandmaster will cause buffer-level drift over long sessions. For short sessions (a few minutes), drift is small and usually not audible. If the listener exposes a "free-run" or "no-lock" mode, enable it when testing without PTP.

**With `--ptp`** the RTP timestamp base comes from `CLOCK_TAI`, which `phc2sys` slews against the PTP grandmaster. Talker and listener agree on the PTP epoch; long-session drift is bounded by PTP sync accuracy rather than crystal drift. See "Phase C" above.

## Known limitations

- Receiver-side SAP passive listen is not wired yet — AOEther receivers still take `--group` / `--dest-ip` explicitly. Auto-bind semantics are TBD.
- No payload-type negotiation — talker emits PT=96 unconditionally, receiver accepts any PT. Controllers negotiate PT through SDP in practice, so this rarely matters.
- L16 encoding path exists in the common module but isn't exposed as a `--format` option yet.
- 44.1 kHz / 88.2 kHz / 176.4 kHz work at the wire level but AES67 deployments overwhelmingly use 48 kHz / 96 kHz / 192 kHz.
- `--transport rtp --format dsd*` is rejected; AES67 is PCM-only. DSD streams remain on Modes 1 / 3.
- Multichannel beyond stereo works but AES67 conventions cap per-stream channel count at 8; higher counts should be split into multiple sessions.
- `--ptp` advertises `:traceable` rather than pinning a specific grandmaster identity (gmid), because extracting gmid from `ptp4l` across processes is fiddly. Controllers that require gmid agreement will need the field filled in; tracked as a follow-up.
