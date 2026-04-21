# AOEther

**Audio over Ethernet, straight into a USB DAC.**

AOEther is an open-source system for transporting multichannel PCM and DSD audio over a network into any USB DAC — from a $20 USB headphone dongle to a $10,000 audiophile DAC — with minimal processing on the receiver side. A small program on a Raspberry Pi (or a Linux SBC, or an MCU) copies samples from Ethernet into the DAC's UAC2 input. No sample-rate conversion. No resampling. No DSP. Just bytes from the network to the DAC.

> **Status:** Pre-implementation. Design is at [v1.2](docs/design.md). M1 (stereo PCM, RPi + USB DAC, no PTP) targets its first working build in one weekend of effort.

## What it does

```
 ┌────────────┐       ┌──────────────┐       ┌──────────────┐
 │  Talker    │       │  AOEther     │       │   USB DAC    │
 │ (Linux PC) │──eth─▶│  receiver    │──usb─▶│ (any UAC2-   │
 │            │       │ (Pi / SBC /  │       │  compliant)  │
 │            │       │  MCU)        │       │              │
 └────────────┘       └──────────────┘       └──────────────┘
```

The receiver enumerates its attached DAC, discovers what it supports (rates, channels, native DSD), advertises those capabilities to the talker, and the talker sends the right format. Because the receiver uses the host OS's stock UAC2 driver (`snd_usb_audio` on Linux), native DSD up to DSD1024 works today with no kernel patches — just a `$60` Raspberry Pi plus whatever USB DAC you already own.

## Why this exists

HQPlayer's NAA for network audio into USB DACs is terrific but closed source and we can't extend it to handle things like PTPv2/multichannel to multiple endpoints etc. Milan is excellent for pro AV but doesn't carry DSD and assumes expensive Milan-certified endpoints. AES67 is IP-based, doesn't target UAC2, and doesn't do DSD either. AOEther fills the gap: **open source, DSD-first, minimum hardware, network-native.**

What this does that existing open-source projects don't:

- **Network audio into USB DACs** directly — no intermediate computer needed.
- **Native DSD up to DSD2048** (hardware permitting), not just PCM or DoP.
- **Deterministic timing** via gPTP / TSN on capable hardware; software PTP fallback everywhere else.
- **Multiple transport modes** — raw Ethernet for wired determinism, IP/UDP for WiFi and routed networks, AVTP for Milan interop, RTP for AES67 interop.
- **Three hardware tiers** from $60 RPi to sub-$50 BOM MCU, all speaking the same protocol.
- **Capability discovery** — receiver tells talker what its DAC supports, streams get formatted accordingly.

## First audio in 30 minutes (M1 preview)

Hardware you need:

- Raspberry Pi 4 or 5 (~$60 with power supply)
- Any USB DAC — a $20 USB headphone dongle works for bring-up; a Topping E30 (~$130), Schiit Modi+ (~$100), or AudioQuest DragonFly (~$100) are nice low-end picks
- Another Linux machine (the talker)
- Cat 6 cable or a dumb gigabit switch

Software steps (condensed — full quickstart in [`docs/quickstart.md`](docs/quickstart.md) once M1 lands):

```sh
# On the Pi (one-time setup)
sudo apt install build-essential libasound2-dev
git clone https://github.com/<your-org>/AOEther.git
cd AOEther/receiver && make

# On your talker machine
git clone https://github.com/<your-org>/AOEther.git
cd AOEther/talker && make

# Plug USB DAC into the Pi. Find its ALSA name:
aplay -L | grep hw:

# Start the receiver, then the talker
sudo ./build/receiver --iface eth0 --dac hw:CARD=Dragonfly,DEV=0
sudo ./build/talker   --iface eno1 --dest-mac <pi-mac> --source testtone
```

You should hear a clean 1 kHz tone.

## Project status and milestones

| Milestone | Status | Description |
|-----------|--------|-------------|
| M1 | Planned | Stereo PCM, RPi + USB DAC, no PTP |
| M2 | Planned | Multichannel PCM (5.1, 7.1, 7.1.4) |
| M3 | Planned | Tier 2 hardware (Linux SBC), hardware PTP |
| M4 | Planned | IP/UDP transport (WiFi and routed networks) |
| M5 | Planned | AVTP AAF wire format for Milan interop |
| M6 | Planned | Native DSD64 through DSD512 end-to-end |
| M7 | Planned | AVDECC, MCU receiver track kickoff |
| M8 | Planned | Full Atmos scale, DSD1024/2048, packaging |
| M9 | Planned | Ravenna / AES67 interop |

See [`docs/design.md`](docs/design.md) for the detailed milestone plan and architecture rationale.

## Hardware tiers

| Tier | Hardware | Target cost | Primary use |
|------|----------|------------:|-------------|
| 1 | Raspberry Pi 4/5 + USB DAC | ~$80 | Listening room, development, home hi-fi |
| 2 | NXP i.MX 8M Mini / Marvell ClearFog / BeagleBone AI-64 | $150–$300 | Hardware-PTP deployments, Milan interop |
| 3 | NXP MIMXRT1170 MCU | $200 eval, sub-$50 BOM | Embedded product-grade streamers |

All three run the same protocol.

## Documentation

- [`docs/design.md`](docs/design.md) — full architecture and milestone plan (start here for reviewers and contributors)
- [`docs/wire-format.md`](docs/wire-format.md) — wire protocol reference (for implementers)
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — how to help

## License

[Apache 2.0](LICENSE). Contributions welcome; see `CONTRIBUTING.md`.

## Credits

Concept, design, and early work: Jonathan (with Claude as design-doc collaborator).

Related work and prior art: [HQPlayer NAA](https://signalyst.com/) (Signalyst), [OpenAvnu](https://github.com/Avnu/OpenAvnu) (Avnu Alliance), [Linux f_uac2](https://github.com/torvalds/linux/blob/master/drivers/usb/gadget/function/f_uac2.c), [snd_usb_audio](https://github.com/torvalds/linux/tree/master/sound/usb).
