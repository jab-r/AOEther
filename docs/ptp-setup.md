# PTP setup for AOEther

This doc covers running `ptp4l` and `phc2sys` on AOEther Tier 1 / Tier 2 hosts so that the talker and receiver(s) share a common time reference. That reference is what AOEther Mode B (M3+) uses to phase-align multiple receivers — see `docs/design.md` §"Clock architecture".

**Scope**: wired LAN only. PTP over WiFi doesn't meaningfully work on commodity hardware; AOEther's Mode C feedback handles single-receiver rate matching over WiFi without PTP, which is the intended answer there.

**Status**: ahead of AOEther code. The talker and receiver binaries do not yet consume `presentation_time` meaningfully. This doc describes the PTP plumbing so that when the code lands, the time source is already working and measurable.

## Two profile choices

PTP has several interoperable profiles. AOEther primarily uses two:

- **gPTP (IEEE 802.1AS-2020)** — Milan and TSN use this. Domain 0, L2 multicast, `delayMechanism P2P`. This is what Mode B assumes by default.
- **PTPv2 default / IEEE 1588-2019** — Ravenna/AES67 setups use this. UDP-over-IP transport (Modes 3/4), commonly on domain 0 or a site-specific domain. AOEther supports it for AES67 interop in M9.

`linuxptp` handles both. Configuration below shows gPTP first, with notes on PTPv2 variations.

## Hardware vs software timestamping

- **Hardware timestamping** (Tier 2, see `tier2-bringup.md`): the NIC captures precise timestamps as packets cross the MAC. `ptp4l` converges to sub-µs offset against a master on a wired LAN.
- **Software timestamping** (Tier 1 Pi, common Intel desktop NICs without PTP support): timestamps are captured by the kernel just above the driver. 10–100 µs accuracy typical; enough for M3's "it works at all" bar but not for sub-µs phase alignment.

Check which you have:

```sh
sudo apt install linuxptp ethtool
ethtool -T eth0
```

Look for `SOF_TIMESTAMPING_TX_HARDWARE`, `SOF_TIMESTAMPING_RX_HARDWARE`, `SOF_TIMESTAMPING_RAW_HARDWARE`. If all three are listed, you have hardware PTP. If only the `SOFTWARE` variants are present, you're in software-timestamp mode.

## Roles: master vs slave

In a single-talker setup, the simplest arrangement:

- **Talker** runs `ptp4l` as the grandmaster (best-master-clock algorithm will elect it if nothing better is on the network).
- **Each receiver** runs `ptp4l` as a slave, syncing its PHC (and via `phc2sys`, its system clock) to the talker.

For production deployments with an existing PTP master (an AVB switch, a dedicated grandmaster, a Milan controller), point everyone at that master instead — the talker becomes a slave too.

## Config files

### Grandmaster (talker side)

`/etc/linuxptp/gPTP-master.conf`:

```ini
[global]
# 802.1AS / gPTP profile
gmCapable               1
priority1               128
priority2               128
logAnnounceInterval     0
logSyncInterval        -3
logMinPdelayReqInterval 0
transportSpecific       0x1
ptp_dst_mac             01:80:C2:00:00:0E
network_transport       L2
delay_mechanism         P2P
domainNumber            0
```

Run it:

```sh
sudo ptp4l -f /etc/linuxptp/gPTP-master.conf -i eth0 -m
```

`-m` prints to stderr; drop it and use systemd for persistent operation.

### Slave (receiver side)

`/etc/linuxptp/gPTP-slave.conf`:

```ini
[global]
# 802.1AS / gPTP profile (slave-capable)
gmCapable               0
priority1               255
priority2               255
logAnnounceInterval     0
logSyncInterval        -3
logMinPdelayReqInterval 0
transportSpecific       0x1
ptp_dst_mac             01:80:C2:00:00:0E
network_transport       L2
delay_mechanism         P2P
domainNumber            0
```

Run it, then discipline the system clock from the PHC:

```sh
sudo ptp4l -f /etc/linuxptp/gPTP-slave.conf -i eth0 -s -m &
sudo phc2sys -s eth0 -c CLOCK_REALTIME -w -m &
# Or, to keep CLOCK_TAI in sync (what AOEther will read), also:
sudo phc2sys -s eth0 -c CLOCK_TAI -O 0 -w -m &
```

On glibc, `clock_gettime(CLOCK_TAI, ...)` reads a clock that `phc2sys -c CLOCK_TAI` disciplines against gPTP (which is TAI-based). The AOEther talker will read this clock to populate `presentation_time` once that code lands.

### PTPv2 default profile (for AES67 / Ravenna interop, M9)

Same tooling, different config. `/etc/linuxptp/PTPv2.conf`:

```ini
[global]
gmCapable               0
priority1               248
priority2               248
logAnnounceInterval     1
logSyncInterval        -3
logMinDelayReqInterval -3
transportSpecific       0x0
network_transport       UDPv4
delay_mechanism         E2E
domainNumber            0
```

Note `UDPv4` transport and `E2E` delay mechanism — these are the AES67 conventions. Run on the same Ethernet interface; you can even run gPTP and PTPv2 on different domains simultaneously if your hardware supports multiple PHC contexts.

## Validation

### 1. `ptp4l` slave locks to master

On the slave, after a few seconds:

```
ptp4l[1234.567]: rms   45 max  182 freq ... delay    50 +/-   5
```

`rms` should be < 1000 ns on a wired LAN with hardware timestamping, < 100 µs with software timestamping. If it doesn't converge, check firewall (allow L2 multicast for gPTP), switch configuration (many managed switches drop PTP by default — enable "PTP transparent" or disable filtering), and that both sides use the same delay mechanism (P2P).

### 2. `phc2sys` locks system clock to PHC

```
phc2sys[1234.567]: CLOCK_REALTIME phc offset   -120 s2 freq ... delay    0
```

`s2` means "servo state 2 = locked". `s0` is the startup state; if it doesn't progress past `s0`, the PHC isn't stable yet.

### 3. Cross-host sanity check

On two slaves, measure their system-clock offset against each other:

```sh
# On slave A
date +%s%N
# On slave B at nearly the same instant
date +%s%N
```

Over `ssh` with aligned polling this is noisy (SSH RTT dominates); for a real measurement, emit a PPS or trigger a GPIO from both slaves on the same gPTP-scheduled tick and scope the skew. Sub-µs between two hardware-PTP-slaved endpoints on a well-behaved switch is the target.

### 4. Under-load stability

Real-world test: play music through AOEther for 1 hour while `ptp4l` is running. The `ptp4l` rms should not degrade meaningfully under network load. If it does, suspect the switch's PTP handling (use a TSN-capable switch for serious deployments) or CPU contention on the slave.

## systemd units (persistent operation)

`/etc/systemd/system/ptp4l.service`:

```ini
[Unit]
Description=PTP4L on eth0 as slave
After=network.target

[Service]
ExecStart=/usr/sbin/ptp4l -f /etc/linuxptp/gPTP-slave.conf -i eth0 -s
Restart=always

[Install]
WantedBy=multi-user.target
```

`/etc/systemd/system/phc2sys.service`:

```ini
[Unit]
Description=phc2sys CLOCK_TAI from eth0 PHC
After=ptp4l.service

[Service]
ExecStart=/usr/sbin/phc2sys -s eth0 -c CLOCK_TAI -O 0 -w
Restart=always

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now ptp4l phc2sys
```

## Troubleshooting

- **`ptp4l` fails with "hardware timestamping not supported"**: either the NIC lacks PTP hardware (check `ethtool -T`), or the kernel driver doesn't wire it up. For i.MX 8M Mini specifically, the `fec` driver historically had PTP quirks; the ENET_QOS path in recent kernels is the good one. Mainline 6.1+ is recommended.
- **Sync jumps around by ~1 ms periodically**: something else on the host is slewing the clock. Disable NTP (`systemctl disable chronyd systemd-timesyncd`) when running `phc2sys` — the two will fight.
- **Master elected on the wrong host**: check BMCA priorities. Lowest `priority1/priority2` wins; make sure the intended master has the smallest values.
- **Works on a cable, fails through a switch**: the switch is dropping PTP. Consumer switches often mangle timestamps (non-transparent). Use a PTP-aware switch (Cisco, NETGEAR with PTP profile, Ruckus, or any AVB-capable switch) for serious measurements.

## What AOEther reads from this

**M9 Phase C (shipped, talker side):** `./build/talker --transport rtp --ptp` reads `CLOCK_TAI` at startup and derives the RTP timestamp base from it. This is the AES67 expectation — RTP timestamps reflect seconds from the PTP epoch on the media clock. The emitted SDP also grows `a=ts-refclk:ptp=IEEE1588-2008:traceable` + `a=mediaclk:direct=0`, so AES67 controllers see a PTP-traceable stream. Once `phc2sys -c CLOCK_TAI` is running, `--ptp` is safe to enable on the talker.

**AOE / AVTP paths:** eventually the talker will also stamp `presentation_time = low32(tai_ns + offset_ns)` into the AoE header for Mode B (phase-aligned multi-receiver). That code has not landed yet; the receiver side will use the same lookup and schedule playback around that target.

If you don't run PTP at all, AOEther continues to work per M1/M2 semantics — Mode C clock discipline handles single-receiver rate matching on its own. RTP/AES67 without `--ptp` will drift against a PTP-locked listener, but short sessions and free-run-capable listeners are unaffected.
