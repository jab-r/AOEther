# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository status

**Pre-implementation.** There is no source code yet — only design documents. The canonical artifacts are:

- `docs/design.md` (v1.2) — architecture, rationale, milestone plan. Source of truth.
- `docs/wire-format.md` (v1.2) — byte-level AoE header and transport encapsulation reference.
- `CONTRIBUTING.md` — style and PR flow.

Any code you write must match what these documents specify. When design and code disagree, update the design doc in the same change — the doc is the canonical record (see CONTRIBUTING.md "Design discussion").

Implementation language is **C** (per CONTRIBUTING.md and the M1 sketch in `design.md`).

## What's being built

A two-binary system plus a future MCU firmware:

- **`talker/`** — Linux userspace. Reads audio from a source (test tone, WAV, ALSA capture), constructs AoE frames, emits them via `AF_PACKET` at 8000 pps aligned to USB microframes. Depends on libc; optionally libasound2 for live capture.
- **`receiver/`** — Linux userspace on Raspberry Pi (Tier 1) or a PTP-capable SBC (Tier 2). Reads AoE frames from `AF_PACKET`, writes samples to an ALSA PCM device backed by `snd_usb_audio`. M1 target is ~100 lines of C. Depends on libasound2.
- **MCU receiver** (Tier 3, from M7) — bare-metal firmware for NXP MIMXRT1170-EVKB. Hand-rolled USB host UAC2 stack; not present in early milestones.

The core architectural commitment (see `design.md` §"Architecture overview" and Appendix B): the receiver is a **USB host** driving a USB DAC (Topology B). Do **not** reintroduce gadget mode (`f_uac2`, UAC2 device emulation) into the primary path — it is deliberately deferred to post-M9 per Appendix C.

### Wire format invariants

Two coexisting protocols:

- **Data frames** — 16-byte AoE header (Magic `0xA0`, Version `0x01`) on EtherType `0x88B5` (L2), AVTP `0x22F0` (Milan), or IP/UDP port 8805 (Mode 3). Only the outer wrapper changes across transport modes; the AoE header itself is identical.
- **Control frames** — 16-byte AoE-C header (Magic `0xA1`, Version `0x01`) on EtherType `0x88B6`. Currently only FEEDBACK frames (type `0x01`) for Mode C clock discipline; see `docs/wire-format.md` §"Control frames". Any new out-of-band signaling should live here, not overloaded onto data frames.

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

No build system exists yet. Once M1 lands there will be `talker/Makefile` and `receiver/Makefile`. Planned entry points (from `design.md` §M1):

```sh
cd talker && make
sudo ./build/talker --iface eno1 --dest-mac <pi-mac> --source testtone

cd receiver && make
sudo ./build/receiver --iface eth0 --dac hw:CARD=<name>,DEV=0
```

Both binaries need `CAP_NET_RAW` (hence `sudo` during development) because they open raw `AF_PACKET` sockets. Required Linux build deps: `build-essential`, `libasound2-dev`. `linuxptp` is added at M3, `libnl-3-dev` later for TSN.

There is no CI config, no `.clang-format`, and no `.editorconfig` yet — CONTRIBUTING.md says these land with M1. Until then, match the style spelled out there manually.

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
