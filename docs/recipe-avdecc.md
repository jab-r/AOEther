# Recipe: AVDECC discovery with Hive

**M7 Phase B.** AOEther receivers and talkers expose themselves as AVDECC entities so Milan controllers like [Hive](https://www.hive.team/) can discover them and establish streams with one click, without hand-editing MAC addresses or stream IDs.

Pairs with [`docs/recipe-milan.md`](recipe-milan.md), which covers the AAF data path — this recipe is about the control plane.

**Step 2 status:** ADP advertising works. Hive and other Milan controllers discover AOEther receivers and talkers and can browse their descriptors (entity name, group name, firmware version). The entity's single configuration is minimal — step 3 expands it with `STREAM_INPUT` / `STREAM_OUTPUT` / `AUDIO_UNIT` / `AVB_INTERFACE` descriptors and wires an ACMP delegate so Hive's "Connect" button actually drives the AOEther data path.

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
        [Phase B step 2 — streams + ACMP handler arrive in step 3]
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

**Entity visible but "Connect" has no effect** — expected through step 2. Step 2 delivers discovery only; step 3 adds ACMP CONNECT handling that drives the AOEther data path. Hive will either show "Stream format not compatible" or simply do nothing when you drag-connect. Until step 3 lands, drive the data path manually with `--dest-mac` on the talker after noting the receiver's MAC from Hive's entity details pane.
