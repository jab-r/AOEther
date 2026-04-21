# Recipe: Milan / AVTP interop

This recipe covers running an AOEther talker as an IEEE 1722 AVTP AAF source so that off-the-shelf Milan / AVB listeners (Hive-controlled MOTU AVB interfaces, L-Acoustics speakers, Avnu-certified audio devices) can receive a stream from AOEther.
For wire-format details see [`wire-format.md`](wire-format.md) §"Mode 2 (AVTP AAF)".

## What works in M5

- Talker emits valid IEEE 1722-2016 AAF PCM frames at EtherType `0x22F0`.
- AOEther receiver consumes those frames and plays them through any ALSA-supported DAC.
- Mode C clock-discipline FEEDBACK frames continue to flow over `0x88B6` between AOEther endpoints. Milan listeners ignore them harmlessly.

## What does NOT work yet

- **No AVDECC.** Milan controllers like Hive will not see AOEther in the discovery panel. Streams must be subscribed manually (instructions below). AVDECC arrives in M7.
- **No MSRP / SRP reservation.** Frames are sent best-effort. On a TSN switch this means traffic may be dropped under contention. A quiet wire or a switch configured to accept AAF on best-effort works.
- **No gPTP-disciplined timestamps.** `tv` is `0` and `avtp_timestamp` is `0`. Listeners that *require* a valid presentation time will reject AOEther's frames; many do not (especially when stream subscription is manual). Real PTP integration arrives in M7 alongside AVDECC.
- **PCM only.** AAF doesn't carry DSD; for DSD use `--transport l2` or `--transport ip` with the DAC matrix in [`dacs.md`](dacs.md).

## Step 1 — verify the bytes look right with Wireshark

Before troubleshooting against a real listener, confirm AOEther is emitting frames Wireshark's AVTP dissector accepts.

```sh
# On any host on the LAN (or the talker itself):
sudo wireshark -i eth0 -k -f 'ether proto 0x22F0'
```

Run the talker against a multicast Milan-range MAC:

```sh
sudo ./build/talker --iface eno1 \
                    --transport avtp \
                    --dest-mac 91:E0:F0:00:01:00 \
                    --source testtone --rate 48000 --channels 2
```

In Wireshark, frames should be classified as `IEEE 1722` and the dissector should display:
- subtype = `02` (AAF)
- format = `03` (INT_24)
- nsr = `5` (48 kHz)
- channels_per_frame = `2`
- bit_depth = `24`
- stream_data_length = `36` (= 6 samples × 2 ch × 3 B at 48 kHz / 8000 pps)
- stream_id = your interface MAC followed by `0001`

If any of those fields is wrong, the bytes will not be accepted by a real Milan listener. Fix the wire-level layout before moving on.

## Step 2 — talker → AOEther receiver over AVTP

Smoke-test the full AVTP path within AOEther itself. Both binaries support `--transport avtp`:

```sh
# Receiver
sudo ./build/receiver --iface eth0 \
                      --transport avtp \
                      --dac hw:CARD=Dragonfly,DEV=0 \
                      --rate 48000 --channels 2

# Talker
sudo ./build/talker --iface eno1 \
                    --transport avtp \
                    --dest-mac <receiver-iface-MAC> \
                    --source testtone --rate 48000 --channels 2
```

You should hear a clean 1 kHz tone. Mode C feedback continues to operate (the receiver banner shows `feedback=on`) — useful as a positive control that the AAF byte-swap and packet sizing are right.

## Step 3 — talker → Milan listener (e.g., Hive + MOTU AVB)

Tested with: *(populate as we get data)*

| Listener | Firmware | Result | Notes |
|---|---|---|---|
| MOTU AVB Stage-B16 | TBD | TBD | Manual stream subscription required |
| L-Acoustics LA4X | TBD | TBD | TBD |

General procedure with Hive:

1. Plug talker and listener into the same wired switch (best-effort or AAF-class queue).
2. Launch [Hive](https://github.com/christophe-calmejane/Hive) on a controller PC on the same LAN.
3. Hive will discover the listener via AVDECC. It will **not** discover AOEther's talker (M7 prereq).
4. In Hive's stream-connection panel, manually create a "stream input" entry for the listener pointing at:
   - Stream ID = AOEther talker's MAC (6 bytes) followed by `0001`
   - Stream destination MAC = whatever you passed to `--dest-mac`
   - Stream format = AAF, INT_24, 48 kHz, *N* channels (must match talker)
5. Activate the subscription on the listener side.
6. Start the AOEther talker.

If audio doesn't flow, check in order:
- `tv` mismatch — some listeners reject `tv=0` outright. Workaround pre-M7: configure listener to "free-run" or "presentation time uncertain" mode if it offers one.
- Sample-format mismatch — listener may default to INT_32 or INT_16. Set it to INT_24.
- VLAN priority — Milan typically uses VLAN priority 3 (Class B) or 5 (Class A). AOEther emits untagged. Configure the switch to remap untagged → priority 3, or add an upstream VLAN tag.
- Multicast filtering — IGMP snooping doesn't apply to L2 multicast, but some switches still flood-restrict the AVTP MAC range. Disable any "AVB unaware" forwarding rules.

## Step 4 — Milan talker → AOEther receiver

Symmetric direction also works. Configure the Milan talker (e.g., a MOTU AVB device acting as a source) to emit AAF at one of AOEther's supported rates (48 kHz / 96 kHz typically) at INT_24, then run AOEther receiver:

```sh
sudo ./build/receiver --iface eth0 \
                      --transport avtp \
                      --dac hw:CARD=ToppingE30,DEV=0 \
                      --rate 48000 --channels 2
```

In this direction Mode C is inactive (the talker doesn't speak our 0x88B6 control frames), so stream stability depends on the Milan talker's gPTP-disciplined media clock. AOEther's receiver still sends FEEDBACK frames; a non-AOEther talker simply ignores them.

## Troubleshooting

**Talker says `AVTP AAF has no NSR code for rate N`** — AAF supports a fixed table of sample rates (8/16/24/32/44.1/48/88.2/96/176.4/192 kHz). AOEther covers everything in its standard rate set, but if you're trying a non-standard rate you'll hit this guard. Use a standard rate or fall back to `--transport l2`.

**Receiver counter `dropped` rises rapidly with `rx=0`** — frames are arriving but the AAF format-specific block doesn't match (channels, NSR, or bit_depth disagrees with the CLI). Compare the talker's banner against the receiver's banner — they must match exactly.

**Frames are emitted but Wireshark shows `Malformed Packet` under the AVTP dissector** — likely a bit-packing bug introduced by a local change to `common/avtp.c`. The reference implementation in `common/avtp.c::avtp_aaf_hdr_build` matches IEEE 1722-2016 Table 18 exactly; revert local changes.

**Audio plays but with periodic clicks/pops on the listener side** — without gPTP, the listener has no synchronization signal and is free-running its own media clock. A short-term fix is to widen the listener's input buffer (most Milan listeners expose a "presentation offset" parameter); the long-term fix is M7 PTP.
