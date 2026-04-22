# Recipe: AVDECC discovery with Hive

**M7 Phase B.** AOEther receivers and talkers expose themselves as AVDECC entities so Milan controllers like [Hive](https://www.hive.team/) can discover them and establish streams with one click, without hand-editing MAC addresses or stream IDs.

Pairs with [`docs/recipe-milan.md`](recipe-milan.md), which covers the AAF data path — this recipe is about the control plane.

**Step 4 status:** ACMP drives the data path.  Hive's Connect button now steers AOEther end-to-end:

- The talker's ACMP observer watches for `CONNECT_TX_COMMAND` targeting its EID, extracts the listener's stream destination MAC, and the per-packet egress path uses that MAC instead of `--dest-mac` until `DISCONNECT_TX` arrives.  Start the talker *without* `--dest-mac` if `--avdecc` is set — it will be idle (emitting to `00:00:00:00:00:00`, which switches drop) until Hive binds it.
- The listener's observer watches for `CONNECT_RX_RESPONSE`, extracts the talker's stream destination MAC, and prefers that MAC over the first-frame-learns fallback.  Strays from other talkers on the same segment no longer hijack the stream.
- `DISCONNECT_TX` / `DISCONNECT_RX` unbind; the next bind fires `on_bind` again.

The C-side glue uses a small mutex-guarded struct so la_avdecc's worker thread can update peer MAC safely while the 8000-pps main loop reads it.

## One-time setup

```sh
# On the Linux machine running either talker or receiver:
sudo apt install cmake g++ libpcap-dev

# Populate the la_avdecc submodule (first time only):
cd /path/to/AOEther
git submodule update --init --recursive

# Build the AVDECC static lib (takes a few minutes on first build):
make -C avdecc

# Rebuild receiver and talker — their Makefiles detect the archive and
# link it in automatically:
make -C receiver clean && make -C receiver
make -C talker   clean && make -C talker
```

Confirm the binaries are AVDECC-capable:

```sh
./receiver/build/receiver --help | grep -i avdecc
./talker/build/talker     --help | grep -i avdecc
```

Both should show the `--avdecc` flag.

## Run with Hive

### Receiver side (listener entity)

```sh
sudo ./receiver/build/receiver --iface eth0 \
                               --dac hw:CARD=D90,DEV=0 \
                               --transport avtp \
                               --avdecc --name "living-room-dac"
```

Banner includes:

```
avdecc: entity up (role=listener name="living-room-dac" iface=eth0 EID=0x....)
        [Phase B step 4 — ACMP wired to data path]
```

When Hive binds the stream you'll see the ACMP trace in the entity's log:

```
avdecc: ACMP CONNECT_RX_RESPONSE → bind peer=91:e0:f0:00:01:00 stream_id=0x0001...
```

### Talker side (talker entity)

```sh
sudo ./talker/build/talker --iface eno1 \
                           --transport avtp \
                           --dest-mac 91:E0:F0:00:01:00 \
                           --source testtone \
                           --avdecc --name "living-room-source"
```

### Hive

1. Launch Hive on the same subnet.
2. Open the controller pane — listener and talker should appear in the entity list under their configured names.
3. Drag-connect the talker's STREAM_OUTPUT to the receiver's STREAM_INPUT, or select both and use the "Connect" button.

When step 2 lands, Hive will drive the AOEther data path through ACMP — the talker learns the receiver's stream destination MAC, the receiver learns the talker's stream ID, and audio starts flowing without any manual CLI flags.

## Relationship to mDNS-SD (Phase A)

`--avdecc` and `--announce` are orthogonal and complementary:

- **mDNS-SD (`--announce`, Phase A)** is the right discovery surface for Mode 3 (IP/UDP) deployments and for any simple-home-network or scripting workflow. It is not visible to Milan controllers.
- **AVDECC (`--avdecc`, Phase B)** is the right surface for Milan-compatible controllers and for any pro-AV workflow where the user expects their controller to manage stream subscription. It is L2-only (uses raw Ethernet under the hood).

Run both on the same receiver if you want — they answer different clients on different protocols and don't interact.

## Troubleshooting

**`make -C avdecc` fails with "la_avdecc submodule not populated"** — run `git submodule update --init --recursive` from the AOEther repo root.

**`make -C avdecc` fails on CMake step** — check la_avdecc's own build requirements (CMake ≥ 3.22, C++17 compiler, libpcap-dev). Their README in `third_party/la_avdecc/README.md` lists full prerequisites.

**`--avdecc` prints "not compiled in"** — you built the receiver or talker before `avdecc/build/libaoether_avdecc.a` existed. Rebuild after the `make -C avdecc` step; the Makefile detects the archive at configure time, not at link time, so a clean rebuild is needed.

**Entity not visible in Hive** — both machines must be on the same L2 segment (AVDECC is not routed through IP gateways); no firewall may drop EtherType `0x22F0` or `0x88B5`; the receiver/talker must be running as root (CAP_NET_RAW) so PCap can open the interface. `enableEntityAdvertising failed (EntityID already in use?)` in the log means another entity on the network is using the same EID — currently AOEther synthesizes EIDs from MAC + role, so two AOEther listeners on the same box collide; use two different interfaces or run only one for now.

**Connect succeeds in Hive but no audio flows** — check that both sides logged an `avdecc: ACMP ... → bind` line.  If the receiver sees the bind but no data frames arrive, the talker might be sending to a MAC the switch can't reach (e.g., a Milan stream-destination multicast on a switch that drops unknown multicasts); try a different stream-destination MAC from Hive's stream-format dialog.  If the talker binds but gets no reply frames, confirm the listener's CONNECT_RX_RESPONSE reached the talker (Wireshark filter `eth.type == 0x22f0 && acmp.message_type == 0x03`).
