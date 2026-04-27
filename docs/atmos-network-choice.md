# AVB vs Ravenna for fixed-array Atmos (7.1.6 / 9.1.6) on M4250

## Context

**Goal:** facilitate **home audiophile multichannel** implementations — listening rooms and home theaters, not pro studios or live venues, with a willingness to invest in good gear but no interest in DAW workflows or pro-tools-of-the-trade complexity.

Earlier AOEther / aether-spatialize thinking flirted with arbitrary speaker geometries, which made the network transport secondary — every output channel was bespoke and the priority was a flexible renderer.
That ambition is now dropped: the target is a **standard Dolby Atmos array** (7.1.6 = 14 ch, 9.1.6 = 16 ch) on a **Netgear AV M4250 switch**.
With the geometry pinned to a published spec, the network choice becomes load-bearing because it determines what speakers/DACs are reachable and how clock discipline propagates to each driver.

The question: AVB (Milan / IEEE 1722 AVTP) or Ravenna (AES67 / RTP)?

**Honest answer: neither is the obvious winner for home audiophile multichannel today; both have credible roles and AOEther keeps both as first-class transports rather than declaring one canonical.**
Ravenna / AES67 (M9 + M10, code-complete) has the slightly broader pool of *audiophile-priced active networked monitors* (Neumann KH AES67 line at ~$2k each, Genelec SAM via Dante-AES67 mode, D&D 8c).
Milan / AVB (M5, shipped) has **native CoreAudio support on every Mac since 2013** and the only turnkey ultra-high-end residential immersive system on the market (L-Acoustics Creations).
Both ecosystems are still small for home use — most home Atmos installs are HDMI-into-AVR, and *any* network-audio multichannel setup is a deliberate, niche choice.

The decision rule keys off **what's already in the home**, not protocol superiority.
The recipes walk through both paths, and the project does not push users toward one transport.

## Genuine consideration

Both ecosystems are immature for home use compared to HDMI-into-AVR.
Within the network-audio niche, here's the honest weighing.

### Where AVB / Milan is actually strong at home

1. **Apple's CoreAudio AVB is native on every Mac since OS X 10.9 (2013).**
   No driver, no licence, no daemon — the Mac is a Milan talker out of the box, with Audio MIDI Setup giving you stream subscription.
   For the dominant home Atmos source (Apple Music spatial audio, Apple TV 4K's spatial output via Logic / Atmos Renderer, or simply a Mac running playback), this is the lowest-friction source-side option in existence.
   The AES67 equivalent requires Dante Virtual Soundcard ($30–$130/year/machine) or a fragile open-source bridge — meaningfully worse UX.
2. **Stream Reservation Protocol (MSRP / SRP, M7 in AOEther's roadmap) gives bandwidth guarantees that AES67 cannot.**
   Home LANs are messy — printers, cameras, a kid's Switch backing up, a Sonos doing its thing.
   AES67 trusts DSCP + IGMP and degrades silently under congestion; Milan's reservations refuse to admit a stream the network can't carry, surfacing a problem instead of dropping samples.
3. **L-Acoustics Creations** (Iseo, Ile, Syva, L-ISA-at-home) is the only turnkey ultra-high-end residential immersive system on the market and is Milan-native.
   AOEther's M5 talker is the right Linux source for that segment.
4. **MOTU AVB interfaces** (16A, 24Ai, etc.) are popular, well-priced multichannel converters with mature drivers and are a reasonable Path-B endpoint.
   AES67-mode equivalents are rarer at MOTU's price point.
5. **Single logical stream per listener** is conceptually simpler than AES67's multi-stream `a=group:LS` SDP bundling, and many AES67 listeners parse the bundling line unevenly (per [`design.md`](design.md), Phase D's first task is *characterising* which listeners actually honor it).
   AVB sidesteps this entirely when the endpoint is one big multichannel DAC.
6. **gPTP has automatic best-master selection** with minimal config; PTPv2 default profile setup is more manual (domain selection, transport choice, BC/E2E mode).
   For a home install that doesn't have an integrator, gPTP is friendlier.

### Where Ravenna / AES67 is actually strong at home

1. **Audiophile-priced active networked monitors lean AES67/Dante.**
   Neumann KH 150 AES67 (~$2.2k each, native AES67), Genelec SAM via Dante-AES67 mode (8341A/8351B, ~$3.2k each), Dutch & Dutch 8c (~$13k/pair, AES67/Dante), Genelec SE7261A/SE7271A subs.
   Equivalent active speakers with *Milan* are essentially nonexistent at consumer prices — Genelec's AVB-capable units are commercial-installation products not retailed to home customers.
2. **PTPv2 default profile is a broader ecosystem standard** outside of Milan-specific deployments — ProAV, broadcast, telco, and any future home gear that adopts network audio is more likely to land on PTPv2.
   gPTP is essentially Milan-only outside of automotive use.
3. **M9 + M10 are code-complete** ([`design.md`](design.md) §M9, §M10) including PTPv2 SDP integration, SAP discovery on both v4 and v6, and multi-stream bundling for 14ch / 16ch splits.
   M5 AVTP is also shipped but doesn't have AVDECC yet (M7), so Milan deployment today requires manual stream subscription via Hive-AE.
4. **Dante's installed base** is enormous, and Dante endpoints typically support AES67 mode, so the *practical* AES67 market is much larger than Ravenna-branded gear alone.
   The M4250 is in fact a Netgear / Audinate co-marketed switch and presets are biased toward Dante.

### Where the two are roughly equal

- **Switch:** M4250 supports both profiles with mature firmware presets for each.
- **Home-room latency:** AES67's 1 ms ptime and AVTP's 125–2000 µs are both far below the threshold for Atmos *playback* (lip-sync to video tolerates 50+ ms; sample-coherence across speakers is the real constraint, and PTP solves it on both sides).
- **AOEther engineering cost:** both transports already exist in the codebase.

### Decision rule

Pick by what's already (or about to be) in the home, in this priority:

1. **L-Acoustics Creations or installed Milan plant → Milan / AVB.**
   Decided.
2. **Mac is the primary source machine and the speaker side is open → Milan / AVB.**
   Source-side friction dominates; CoreAudio native AVB into MOTU AVB or Avid MTRX → analog → amps is a clean stack the user can set up themselves.
3. **Speaker side is decided as Neumann KH AES67 / Genelec SAM / D&D 8c → Ravenna / AES67.**
   No equivalent Milan-native option at audiophile-home pricing exists; speaker dictates network.
4. **Linux/streamer source (Roon, MPD, Plex, AOEther talker) and speaker side is open → toss-up.**
   Lean AES67 for the broader endpoint pool, lean Milan if MSRP guarantees matter for a complex home LAN.
5. **Greenfield, no committed gear → Milan / AVB is the slightly more defensible default for home audiophile** because the source-side story (Mac native) is the single biggest UX advantage and beats Ravenna's slightly-broader-monitor-pool.

## Three parallel deployment paths

Endpoint topology in either Milan or AES67 case is one of:

- **Path A** (active networked monitors): per-speaker DSP + amp + driver in one cabinet, Cat-6 + power per box.
  Genelec SAM / Neumann KH AES67 / D&D 8c on the AES67 side; effectively no audiophile-priced equivalent on the Milan side today.
- **Path B** (one multichannel DAC + amps + passive speakers): MOTU AVB, Avid MTRX, RME M-32, Merging Hapi MkII.
  Available on either transport.
  Right answer if the user wants to keep audiophile-grade passive speakers and matched amplification.

A third path applies orthogonally to the AVB/Ravenna choice and is the only option if the source material is multichannel DSD:

- **Path III: AOE native (Modes 1 / 3) into a multichannel DSD DAC.**
  AOEther's native L2 (`--transport l2`) and IP/UDP (`--transport ip`) modes carry native DSD64–DSD2048 in the AoE header (format codes `0x30..0x35`); M8 packet splitting handles the multichannel payload-size jump (DSD512 stereo is 2 fragments/microframe; multichannel scales linearly).
  Endpoints are the small set of multichannel DSD DACs that exist as USB UAC2 devices: Exasound e62 / e68 (8-ch DSD), Merging NADAC 8 (8-ch DSD), and a few others.
  One AOEther receiver drives one such DAC over USB; Mode C feedback disciplines the talker to that DAC's free-running DSD master clock.
  Multichannel SACD rips (5.1 DSD) and native multichannel DSD recordings work end-to-end.
  AVTP (Mode 2) and RTP/AES67 (Mode 4) are PCM-only on the wire and cannot carry DSD; the AVB-vs-Ravenna choice does not apply to a DSD signal path.

### Path I: Milan / AVB (M5)

For Mac-sourced rigs, L-Acoustics Creations, or any case where MSRP bandwidth guarantees / native CoreAudio source-side wins.

- **Source side:** Mac CoreAudio native (Audio MIDI Setup → AVB stream subscription) *or* Linux running AOEther `talker --transport avtp --dest-mac <listener-mac>`.
- **Endpoint:** L-Acoustics Creations system (managed via L-ISA Controller); MOTU AVB / Avid MTRX → analog → amps; AOEther receiver `--transport avtp` on a Pi/SBC into a USB DAC for hobbyist Path-B builds.
- **Switch:** M4250 in **AVB profile** — gPTP (802.1AS) automatic, MSRP/MVRP enabled.
  The Audio Video Bridging UI in the M4250 firmware is mature and turnkey.
- **Discovery / control:** Hive-AE (open-source AVDECC controller) until M7 lands AVDECC in AOEther itself.
  Manual stream subscription via Audio MIDI Setup on Mac listeners.
- **Code path today:** M5 shipped.
  Milan listeners ignore Mode C feedback (EtherType `0x88B6`) so it can run alongside but does nothing useful when the listener is a Milan endpoint with its own gPTP-disciplined converter.
  M7 closes the AVDECC gap.
- **Channel/rate fit:** AVTP AAF's 10-bit `channels_per_frame` field handles 14/16 ch trivially at 48/96 kHz.
  Single packet per microframe; no fragmentation concerns.
  PCM only — DSD continues on Modes 1/3 if the user wants stereo DSD alongside the multichannel PCM rig.

See [`recipe-milan.md`](recipe-milan.md) for the existing single-stream walkthrough; a multichannel-specific `recipe-multichannel-milan.md` will extend it for the 14/16-ch case.

### Path II: Ravenna / AES67 (M9 + M10)

For Linux-sourced rigs, Neumann/Genelec/D&D-decided speaker stacks, or any case where the user's preferred monitors are AES67/Dante-only.

- **Source side:** Linux running AOEther `talker --transport rtp --ptp --channels-per-stream 8 --dest-ip 239.X.X.10 --announce-sap`.
  Mac source via Dante Virtual Soundcard (~$30/y) is possible but a meaningful UX downgrade vs. the Milan path.
- **Endpoint shortlists** (Path A — active networked monitors):
  - **Entry — stereo + sub, ~$5k.**
    Neumann KH 150 AES67 pair + KH 750 AES67 sub.
    Validates the chain with two endpoints; later expansion adds C, surrounds, heights without revisiting the network.
  - **Standard — full 7.1.6 with Neumann, ~$25–35k.**
    KH 150 AES67 mains + KH 120 II AES67 surrounds/heights + KH 750 AES67 sub(s).
    MA 1 calibration spans the whole array.
  - **Reference — Genelec SAM, ~$40–60k.**
    8341A or 8351B mains + 8331A surrounds/heights + 7370A or 7380A subs via GLM.
    Dante/AES67 mode.
  - **Reference / cardioid — Dutch & Dutch 8c, ~$13k/pair for fronts**, paired with KH-line or other AES67 surrounds.
- **Switch:** M4250 in **Dante / AES67 profile** — IGMP querier + snooping on, PTPv2 default profile (boundary clock if firmware supports it; otherwise a dedicated grandmaster — see below), DSCP map with PTP=EF(46) and audio=AF41(34).
- **Discovery / control:** SAP-announced bundled SDP from the talker, or a static SDP file consumed via each monitor's controller (GLM, MA 1, manufacturer web UI).
- **Code path today:** M9 Phases A–C and M10 Phases A–B shipped on `feature/m9-rtp-aes67`.
  The M10 receiver-side multi-stream reassembler is partial / hardware-blocked but doesn't matter for Path A — each monitor is its own listener.
- **Grandmaster note:** the M4250's PTPv2 BC support varies by firmware; verify before relying on it.
  Otherwise use a Mellanox Spectrum or a Linux box with a hardware-PTP NIC (e.g., Intel I210/I225) running `ptp4l` in master mode.
  [`ptp-setup.md`](ptp-setup.md) already covers the daemon recipe.
- **Channel/rate fit:** 14/16 ch fits cleanly via M10's 8+6 / 8+8 substream split with `a=group:LS` SDP bundling.
  PCM only at 48/96 kHz; DSD remains on Modes 1/3.

See [`recipe-aes67.md`](recipe-aes67.md) for the existing single-stream walkthrough; a multichannel-specific `recipe-multichannel-aes67.md` will extend it for the 14/16-ch case.

### Mixed deployments

Mac source + AES67 monitors *can* be made to work by routing Mac audio through Dante Virtual Soundcard or a small Linux bridge running AOEther in receiver-from-Mac → talker-AES67 mode, but each bridge introduces a clock-domain transition the audiophile is paying not to have.
If the source machine is Mac and the user is shopping for monitors, **let it dictate Milan-compatible endpoints** and avoid the bridge.
If the monitors are already Neumann/Genelec/D&D and the source is Mac, accept the Dante VSC tax or move the source to Linux.
Don't try to run both transports on the same VLAN; if both must coexist (e.g., Atmos rig + a separate networked recording setup), give them separate VLANs on the M4250.

## Endpoint topologies that don't work and why

Some configurations look attractive but break the clock-coherence story that justifies network audio at home in the first place.
Both transports share these limits.

- **Multiple AOEther + USB-DAC receivers, one per channel pair (or per channel).**
  Tempting because it's all-AOEther and lets audiophiles keep their existing stereo DAC(s) in the rig.
  But it lands in the M11 Path C problem: a USB DAC's internal crystal is free-running.
  In M9 RTP mode Mode C feedback is disabled because the talker is PTP-clocked, not slaved to a listener; in M5 AVTP mode Mode C feedback works but only for *one* listener at a time, so the second / third / Nth USB-DAC receiver still free-runs.
  PTP at the Ethernet edge does not propagate to MCLK at the converter in either case.
  Across 8+ such endpoints the array goes incoherent over a listening session — front L/R drifts relative to surrounds and heights in ways that are audible on transient material and panning sweeps.
  M11 documents the PHC-slaved DIY endpoint as the eventual fix but it's open and community-driven.
  Use this topology only for stereo-only listening alongside the multichannel rig (the M1 case); not for full multichannel.
- **Hybrid: networked active monitors for surrounds/heights + audiophile USB DAC for front L/C/R.**
  Same clock-coherence problem applied to the most-noticeable channels (front imaging).
  Don't recommend; if the user insists on keeping their stereo DAC, the right pattern is to use it for stereo-only listening and have a second, separate networked rig for multichannel — not to mix the two.
- **Mixing Milan and AES67 endpoints on one VLAN.**
  Profile-incompatible at the switch (gPTP vs PTPv2 default profile, MSRP vs DSCP).
  Use separate VLANs if both must coexist; don't try to bridge.

## What this means for AOEther

- **Both M5 (AVTP/Milan) and M9+M10 (RTP/AES67) stay first-class.**
  Neither gets demoted.
- **Two follow-up recipes**: `recipe-multichannel-milan.md` and `recipe-multichannel-aes67.md`, each walking through endpoint shortlists, M4250 profile setup, PTP daemon config, talker invocation, and per-channel listener subscription end-to-end for the 14/16-ch case.
- **Drive M7 forward.**
  AVDECC support is the missing piece for Milan to be operationally as easy as AES67's SAP discovery — without AVDECC, Milan deployment requires manually entering stream IDs into Hive-AE, which is a real UX gap.
  M7 closes it.

## Verification

Each path validates separately.
Steps that don't require physical speakers/DACs run today against open-source references.

**Path II (Ravenna / AES67):**

1. **Static SDP sanity.**
   `talker --transport rtp --ptp --channels 14 --channels-per-stream 8 --rate 48000 --format s24le --sdp-only` — confirm the printed SDP contains two `m=audio` sections, `a=group:LS 1 2`, session-level `a=ts-refclk:ptp=IEEE1588-2008:...`, per-`m=` `c=` multicast groups.
   No hardware.
2. **`aes67-linux-daemon` interop.**
   Talker on one Linux box, `aes67-linux-daemon` on a second; confirm SAP discovery picks up the bundled session and a single-substream subscription decodes audio.
   Validates M9 Phases A–C and M10 Phases A–B against an open-source reference.
3. **PTPv2 grandmaster + M4250 BC.**
   `ptp4l` master on a PTP-capable NIC (Intel I210/I225) or M4250 BC if firmware supports it.
   `pmc -u -b 0 'GET CURRENT_DATA_SET'` shows consistent GM; SDP `a=ts-refclk` line updates on forced BMCA event.
4. **First-monitor commissioning.**
   Single Neumann KH 150 AES67 or Genelec 8341A — discovers talker via SAP, plays assigned channel from a sine sweep / per-channel ID tone, 24-h soak shows no drift / no glitches.
   Closes M9 Phase D and M10 Phase D for Path A simultaneously.

**Path I (Milan / AVB):**

1. **AVTP frame sanity.**
   `talker --transport avtp --dest-mac <listener> --channels 16 --rate 48000 --format s24le` — capture with `tcpdump -i <if> ether proto 0x22F0 -w avtp.pcap`, decode AAF header in Wireshark (or with the AVTP Wireshark dissector), confirm correct channel count, NSR, big-endian sample order.
   No specialized hardware.
2. **Mac CoreAudio listener.**
   Mac on the same M4250 with AVB profile active — Audio MIDI Setup → "Audio Devices" → AVB stream tab should list AOEther's stream by entity ID once M7 ships AVDECC.
   Until M7, manual subscription via Hive-AE on a Linux controller works against `feature/m5-avtp-aaf` talker.
   Verifies M5 wire format against a known-good Milan listener.
3. **gPTP grandmaster.**
   `ptp4l -i <if> -f /etc/linuxptp/gPTP.cfg -m` with `transportSpecific 1`, `domainNumber 0` — verify GM election, sub-µs sync to the M4250.
4. **First commercial Milan endpoint.**
   MOTU 16A or Avid MTRX → confirms AVTP AAF stream subscribes and plays.
   Sine-sweep / ID-tone validation as above.
   Closes M5 Phase D.

## Out of scope

- Atmos object rendering — content arrives at the talker as channel-based PCM.
  The aether-spatialize sibling project handles HOA decode / room correction; AOEther transports the result.
- Bass management and per-driver room correction — handled by GLM (Genelec) or MA 1 (Neumann) at each monitor, not in AOEther.
- Multichannel DSD is in scope.
  Single-multichannel-DAC DSD over Modes 1/3 (Path III above) is supported today via M6 + M8 packet splitting.
  Distributed multichannel DSD across multiple endpoints is architecturally no harder than distributed multichannel PCM — both face the same drift problem with commodity free-running USB DACs and have the same answer in M11 Path C hardware (a PHC-slaved fractional-N PLL clocks an I2S PCM or native-DSD output identically; the divider ratio is the only difference).
  See [`design.md`](design.md) §M10 "Documented constraints" and §"Bandwidth budget" for the per-rate / per-channel-count link-speed table.
- AVDECC / Milan control plane — independent milestone (M7), not work-product of this decision, but called out above as a UX-priority follow-up because it's what makes Milan operationally as easy as AES67's SAP discovery.
