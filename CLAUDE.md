# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository status

**M1–M4 merged to main; M5 (AVTP AAF) on its feature branch.** Canonical artifacts:

- `docs/design.md` (v1.4) — architecture, rationale, milestone plan. Source of truth when in conflict with the code.
- `docs/wire-format.md` (v1.4) — byte-level reference for the AoE header, AAF header, and AoE-C control header.
- `CONTRIBUTING.md` — style and PR flow.
- `docs/recipe-*.md` — operator-facing recipes for music sources (Roon, UPnP, PipeWire) and transport modes (Milan/AVTP).

When design and code disagree, update the design doc in the same change — the doc is the canonical record (see CONTRIBUTING.md "Design discussion").

Implementation language is **C** (per CONTRIBUTING.md). MCU firmware (Tier 3, M7+) will also be C.

## What's being built

A two-binary system plus a future MCU firmware:

- **`talker/`** — Linux userspace. Reads audio from a source (test tone, WAV, ALSA capture), constructs AoE frames, emits them via `AF_PACKET` at 8000 pps aligned to USB microframes. Depends on libc and libasound2 (the latter for the capture source).
- **`receiver/`** — Linux userspace on Raspberry Pi (Tier 1) or a PTP-capable SBC (Tier 2). Reads AoE frames from `AF_PACKET`, writes samples to an ALSA PCM device backed by `snd_usb_audio`. M1 target is ~100 lines of C. Depends on libasound2.
- **MCU receiver** (Tier 3, from M7) — bare-metal firmware for NXP MIMXRT1170-EVKB. Hand-rolled USB host UAC2 stack; not present in early milestones.

The core architectural commitment (see `design.md` §"Architecture overview" and Appendix B): the receiver is a **USB host** driving a USB DAC (Topology B). Do **not** reintroduce gadget mode (`f_uac2`, UAC2 device emulation) into the primary path — it is deliberately deferred to post-M9 per Appendix C.

### Music sources are bridged, not native

From M1 onward, AOEther does NOT implement RAAT (Roon), UPnP MediaRenderer, AirPlay, or Spotify Connect natively. These ecosystems are reached by running the appropriate existing daemon (RoonBridge, gmrender-resurrect / rygel, shairport-sync, librespot) on the talker box, having it write to one half of a `snd-aloop` kernel loopback, and running the AOEther talker with `--source alsa --capture hw:Loopback,1,N` on the other half. Recipes live in `docs/recipe-*.md`. This pattern is deliberate — it covers every mainstream Linux music source at effectively zero code cost on our side and inherits Mode C's DAC-clock propagation all the way upstream (snd-aloop's shared ring transmits back-pressure to the source daemon's ALSA write side). Do not add native protocol implementations for these ecosystems without an explicit design-discussion issue that explains why the bridge pattern no longer suffices.

### Wire format invariants

Three coexisting protocols on the wire:

- **AOE data frames** — 16-byte AoE header (Magic `0xA0`, Version `0x01`) on EtherType `0x88B5` (L2) or IP/UDP port 8805 (Mode 3). The AoE header itself is identical across modes; only the outer wrapper varies.
- **AVTP AAF data frames** (Mode 2, M5+) — 24-byte IEEE 1722 AAF header on EtherType `0x22F0` for PCM streams when `--transport avtp` is used. Format codes / NSR / channels are AAF-native, **not** AOE format codes. Samples are big-endian on the wire (AOE wrappers carry ALSA-native little-endian); `common/avtp.c::avtp_swap24_inplace` handles the byte-swap on the AVTP edge. DSD streams continue to use Mode 1 / Mode 3.
- **Control frames** — 16-byte AoE-C header (Magic `0xA1`, Version `0x01`) on EtherType `0x88B6`, used by Mode C clock-discipline FEEDBACK regardless of which data transport is active. Milan listeners ignore unknown EtherTypes, so AOEther's feedback loop is invisible to them. Any new out-of-band signaling should live here, not overloaded onto data frames.

Multi-byte fields are big-endian. Format codes, flag bits, control-frame value encodings, and the AAF byte layout are enumerated in `docs/wire-format.md` — treat that file as the authoritative spec; don't redefine codes locally.

### AVDECC and Milan control-plane scope

AOEther's M5 ships AVTP **data-plane** interop only. AVDECC (the IEEE 1722.1 entity model that lets Hive and other Milan controllers discover and bind streams), MSRP (stream reservation), and gPTP-disciplined `avtp_timestamp` are deliberately deferred to M7. Until then, Milan-listener subscription is manual (see `docs/recipe-milan.md`). Do not start an AVDECC implementation under a different milestone — it has design implications for the discovery layer that need to be considered alongside mDNS-SD (IP discovery) all at once in M7.

Multi-byte fields are big-endian. Format codes, flag bits, and control-frame value encodings are enumerated in `docs/wire-format.md` — treat that file as the authoritative spec; don't redefine codes locally.

### Clock architecture (v1.3)

The system extends USB UAC2 asynchronous feedback across Ethernet:

- `snd_usb_audio` on the receiver already slaves ALSA consumption to the DAC via UAC2 async feedback locally.
- The receiver then re-expresses that consumption rate as a Q16.16 samples-per-ms value (exactly UAC2 HS feedback format) and sends it to the talker in a FEEDBACK control frame every ~20 ms.
- The talker runs a fractional sample accumulator driven by the latest FEEDBACK value. **Packet cadence stays constant; `payload_count` varies per packet** — the integer part of the accumulator. This mirrors how a USB host adjusts microframe payload size under UAC2 async.

This is **Mode C** and is the baseline operating mode from M1 onward. **Mode A** (open-loop, fixed samples-per-packet) is diagnostic only — it fails within seconds under real crystal drift. `--no-feedback` on the receiver exists specifically as a positive control for soak tests. **Mode B** (gPTP-disciplined emission, M3+) layers on top of Mode C for multi-receiver phase alignment; it does not replace Mode C because consumer USB DACs are free-running crystals that nothing in the network disciplines.

Non-negotiable data-path rules (from `design.md` Goals / Non-goals):

- No sample-rate conversion, ever. If talker and DAC disagree on format, fail stream setup cleanly.
- No per-sample DSP, no mixing, no effects in the receiver.
- Jitter buffer on the receiver; underrun substitutes silence, never blocks.
- The talker's `timerfd` **never retunes** under Mode C — drift is absorbed by per-packet payload size, not timer period. If you find yourself wanting to adjust the timer by ppm increments, you're solving the wrong problem.

## Commands

```sh
cd talker && make            # → build/talker
cd receiver && make          # → build/receiver

# Default L2 (raw Ethernet) — M1 baseline:
sudo ./build/receiver --iface eth0 --dac hw:CARD=<name>,DEV=0
sudo ./build/talker   --iface eno1 --dest-mac <pi-mac> --source testtone

# IP/UDP transport (M4) — IPv4 / IPv6, unicast or multicast:
sudo ./build/receiver --iface eth0 --dac hw:... --transport ip --port 8805 --group 239.10.20.30
sudo ./build/talker   --iface eno1 --transport ip --dest-ip 239.10.20.30 --source testtone

# AVTP AAF transport (M5) — Milan interop:
sudo ./build/receiver --iface eth0 --dac hw:... --transport avtp
sudo ./build/talker   --iface eno1 --transport avtp --dest-mac 91:E0:F0:00:01:00 --source testtone
```

Both binaries need `CAP_NET_RAW` (hence `sudo` during development) for raw `AF_PACKET` sockets in L2 / AVTP modes. IP mode binds an unprivileged UDP port. Required Linux build deps: `build-essential`, `libasound2-dev`. `linuxptp` is added at M3 Phase B (hardware PTP); `libnl-3-dev` later for TSN.

There is no CI config, no `.clang-format`, and no `.editorconfig` yet. Match the style spelled out in CONTRIBUTING.md manually.

## Code style (from CONTRIBUTING.md)

- K&R brace style, **4-space indent, not tabs**, snake_case for functions/variables, SCREAMING_SNAKE_CASE for macros.
- Header guards via `#pragma once` (not `#ifndef` guards).
- Split a module when it exceeds ~500 lines.
- Shell scripts: `#!/bin/sh`, POSIX, `set -eu`, quote all expansions.
- Markdown: **one sentence per line** in source files so diffs are reviewable.

## Commit and PR conventions

- Imperative mood, 72-char subject, optional body separated by a blank line, reference issues by number.
- **Never add Co-Authored-By lines to commits** (user's global rule).
- Non-trivial changes: open an issue first. Small bug fixes can go straight to PR.
- Branch naming: `feature/<desc>` or `fix/<desc>` off `main`.
- Protocol / wire-format / milestone-plan changes require an issue labeled `design-discussion` and a matching update to `docs/design.md` in the same PR.
