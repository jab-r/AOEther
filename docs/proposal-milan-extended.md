# Proposal: AOEther-Extended Milan profile for 384 kHz PCM and DSD-over-DoP

**Status:** draft for `design-discussion`.
Not implemented; no code exists for any of this yet.

## Context

The home-audiophile multichannel target documented in [`atmos-network-choice.md`](atmos-network-choice.md) lists 384 kHz PCM and multichannel DSD as Path III concerns — handled today only via AOE native Modes 1/3 into a single multichannel DAC over USB.
Neither AVB/Milan nor stock AES67 reaches 384 kHz or carries DSD; the user wanting either has to leave their network-audio transport behind.

Merging Technologies sidesteps this on the AES67/Ravenna side via two *unofficial-but-shipping* extensions to the standard:

1. **DXD-rate PCM (352.8 kHz / 24-bit)** — emitted with the rate declared in the SDP `a=rtpmap` line.
   RTP doesn't constrain the rate at the encoding layer, so this Just Works for any listener that parses arbitrary integer rates from SDP.
2. **DSD over RTP via DoP** — DSD bits packed into 24-bit PCM samples with `0x05`/`0xFA` marker bytes; the wire stays L24 PCM, only the receiver semantics differ.
   Standard at the encoding layer; non-standard at the application layer.

Together these reach DSD64 / DSD128 / DSD256 multichannel and 352.8 kHz PCM over a Ravenna fabric, end-to-end PTP-disciplined, between Merging endpoints (Pyramix → NADAC, Hapi MkII, Horus).
No IETF / AES standard blesses this; it works because Merging owns enough of the producer-and-consumer ecosystem to coordinate the convention.

This proposal asks: **can AOEther do the analogous thing for Milan/AVB**, so that a user who has standardized on Milan as their network-audio transport (Mac CoreAudio source side, AVDECC discovery via Hive, gPTP-disciplined timing, MSRP reservations) can also reach 384 kHz PCM and DSD over the same wire, between AOEther-aware endpoints?

## Honest framing

This is **AOEther-to-AOEther only**.

- No commercial Milan listener will honor what's proposed here.
  Avid MTRX, MOTU AVB, Focusrite RedNet, L-Acoustics Creations, and Apple CoreAudio AVB are all engineered to a Milan baseline that caps at 192 kHz / 24-bit PCM and rejects unfamiliar AAF stream formats at AVDECC binding time.
  A Milan-certified listener that received an AAF stream with NSR=0 (user-specified) would not bind it.
- This is **not** a path to Avnu Milan certification.
  Avnu's working group is driven by automotive and pro-AV vendors with no demand signal for >192 kHz or DSD.
- AOEther's existing **Modes 1/3** (L2 raw / IP/UDP with the AoE header) already carry arbitrary rates and native DSD trivially.
  Anything this proposal delivers can be delivered today by routing the high-rate / DSD content through Modes 1/3 instead.
  The only new capability is **transport consistency** — one Milan-shaped stream story across all rates and formats — for users who have committed to Milan as their network audio fabric.

So: *technically reachable, commercially absent, and architecturally redundant with Modes 1/3.*
The reason to implement it anyway is "audiophile who wants a Milan-everywhere story including DSD" — a real but small audience.
This proposal is for design discussion; whether to actually ship it is a follow-on call.

## What gets reached, and how

| Format | Wire rate | Wire format | NSR mechanism | AVDECC role |
|---|---|---|---|---|
| 192 kHz PCM (already shipped) | 192 kHz / 24-bit | AAF INT_24 | NSR=0x09 (standard) | Existing M5 + M7 Phase B |
| 352.8 kHz PCM (DXD-rate) | 352.8 kHz / 24-bit | AAF INT_24 | NSR=0x00 (user-specified) | New: rate declared in stream-format descriptor |
| 384 kHz PCM | 384 kHz / 24-bit | AAF INT_24 | NSR=0x00 (user-specified) | New: as above |
| 705.6 / 768 kHz PCM | 705.6 / 768 kHz / 24-bit | AAF INT_24 | NSR=0x00 (user-specified) | New: as above; bandwidth-tight |
| DSD64-via-DoP | 176.4 kHz / 24-bit | AAF INT_24 (DoP-encoded) | NSR=0x08 (standard) | New: receiver demodulates DoP |
| DSD128-via-DoP | 352.8 kHz / 24-bit | AAF INT_24 (DoP-encoded) | NSR=0x00 (user-specified) | Combines high-rate + DoP |
| DSD256-via-DoP | 705.6 kHz / 24-bit | AAF INT_24 (DoP-encoded) | NSR=0x00 (user-specified) | Combines high-rate + DoP; jumbo frames |
| DSD512-via-DoP | 1411.2 kHz / 24-bit | (effectively impossible) | — | Skip; out of scope |

Two orthogonal mechanisms cover everything:

- **NSR=0 + AVDECC** — for any non-baseline rate.
- **DoP-encoded AAF** — for DSD packed into AAF INT_24 PCM frames at the appropriate "DoP rate" (= 4 × DSD multiple of 44.1).

DSD64-over-DoP at 176.4 kHz uses a *standard* AAF NSR code, so it could in principle ship without the high-rate extension.
DSD128 and DSD256 over DoP require both mechanisms together.

## Phases

### Phase A — High-rate PCM via NSR=0 + AVDECC

**Goal:** AOEther talker emits AAF AAF streams at 352.8 / 384 / 705.6 / 768 kHz that an AOEther receiver bound via AVDECC accepts and plays.

**Talker side ([talker/src/talker.c](talker/src/talker.c) + [common/avtp.c](common/avtp.c)):**

- Extend `avtp_aaf_nsr_from_hz()` to return NSR=0x00 for unknown rates instead of failing with -1.
- Carry the actual rate in a separate field on the talker's stream-state struct so AVDECC's stream-format advertisement can declare it.
- Re-validate the per-packet payload against MTU at the chosen rate × channel count × sample size; reject startup configs that would exceed the configured MTU (jumbo or otherwise) rather than silently truncating.

**AVDECC side (`avdecc/...`):**

- Extend the `STREAM_OUTPUT` / `STREAM_INPUT` descriptor's `formats[]` array to include entries with NSR=0 and the actual rate annotated in a vendor-private subfield (or a custom `ASSOCIATION_ID` / per-stream descriptor extension; see Open Question #2).
- Hive will display these as opaque "vendor-specific" formats; that's fine because AOEther-to-AOEther binding doesn't need Hive to interpret them.
- AOEther's own ACMP observer reads the rate from the descriptor on `CONNECT_TX_COMMAND` / `CONNECT_RX_RESPONSE` and passes it to the data-plane.

**Receiver side ([receiver/src/receiver.c](receiver/src/receiver.c)):**

- On AVDECC bind to a stream advertising NSR=0, read the actual rate from the bound stream's stream-format descriptor.
- Configure the ALSA PCM device with the bound rate; reject if `snd_pcm_hw_params_set_rate()` fails (DAC doesn't support the rate).
- Mode C feedback continues unchanged.

**Bandwidth check:**

- 384 kHz × 16-ch × 24-bit at AAF Class A 8000 pps gives 48 samples × 16 × 3 = **2304 bytes payload** per packet.
  Plus AVTP/AAF/Eth headers, total ~2350 bytes — exceeds standard 1500 MTU.
  Either jumbo frames (9000 MTU; M4250 supports this in AVB profile) or split across multiple parallel AVTP streams.
- AOEther has no Milan-side equivalent of M10's RFC 5888 LS bundling — that's an AES67 SDP feature.
  Multi-stream Milan would need a separate bundling convention (see Open Question #4).
  Simplest first cut: require jumbo frames for >192 kHz multichannel; document as such.

### Phase B — DoP encoder for DSD over AAF

**Goal:** Talker emits AAF INT_24 streams whose payload is DoP-encoded DSD; receiver demodulates and writes to ALSA either as native DSD (preferred) or DoP-passthrough PCM (fallback).

**DoP modulator (NEW: `common/dop.c` / `common/dop.h`):**

- Functions `dop_pack_dsd64(...)`, `dop_pack_dsd128(...)`, `dop_pack_dsd256(...)` that take 1-bit-per-sample DSD streams (already AOEther's native wire format on Modes 1/3) and pack 16 DSD bits into the lower 16 bits of each 24-bit PCM sample, with the upper byte alternating `0x05` / `0xFA` per sample to mark the frame as DoP.
- Output rate = DSD rate / 16 bits-per-sample = DSD64 → 176.4 kHz, DSD128 → 352.8 kHz, DSD256 → 705.6 kHz.
- Reuse AOEther's existing DSD source plumbing (`audio_source_dsf.c`, `audio_source_dff.c`, `audio_source_dsdsilence`) — the DoP modulator sits between the source and the AVTP emit path.

**Talker integration ([talker/src/talker.c](talker/src/talker.c)):**

- `--transport avtp --source dsf --file foo.dsf --dop` — DoP-encode the DSD stream and emit as AAF at the appropriate DoP rate.
- Reject `--transport avtp --format dsd*` *without* `--dop` at startup — AVTP AAF cannot carry native DSD; the user has to opt into DoP explicitly.
- Wire format codes `AOE_FMT_DOP_DSD64..DSD256` ([common/packet.h:20-22](common/packet.h:20)) are already reserved in AoE; AVTP emits AAF INT_24 with the DoP rate, and the format-code mapping is talker-internal.

**Receiver integration ([receiver/src/receiver.c](receiver/src/receiver.c)):**

- DoP marker scan on first N samples after bind: if `0x05` / `0xFA` alternation is detected in the upper byte, treat the stream as DoP-encoded.
- Two output modes:
  1. **Passthrough** (default for DoP-aware DACs): forward the L24 PCM samples to ALSA at the DoP rate; the USB DAC unpacks DoP itself.
     Most modern DSD DACs (RME ADI-2, Topping, Holo Audio, Denafrips, etc.) recognize DoP markers.
  2. **Demodulate** (for native-DSD-only DACs): unpack the lower 16 bits of each PCM sample back to a DSD bitstream; write to ALSA as `SND_PCM_FORMAT_DSD_U8` at the native DSD rate.
     Receiver-side flag `--dop-demod` selects this; default is passthrough because DoP-aware is the common case.
- Mode C feedback works unchanged (the rate telemetry is just the L24 PCM rate; the DAC's actual consumption rate is what gets reported).

### Phase C — "AOEther-Extended Milan" profile spec

**Goal:** Document the conventions so a future implementer (or a future-AOEther after maintainer turnover) can read the spec and rebuild the extension without re-deriving it.

**Spec doc additions:**

- `docs/wire-format.md` §"Mode 2 (AVTP AAF) — Extended profile (AOEther-private)" — covers NSR=0 conventions, DoP marker layout, multi-stream bundling (if Open Question #4 resolves toward bundling).
- `docs/design.md` §M12 (next available milestone slot) entry covering this proposal's scope, dependencies (M5 + M7 Phase B), and the explicit non-goal of Avnu certification.
- `docs/recipe-milan-extended.md` — operator-facing how-to for AOEther-to-AOEther 384 kHz / DSD over AAF, with the explicit "this only works between AOEther endpoints" warning at the top.

**Naming.**
Working title: "AOEther-Extended Milan profile."
Could also be "AOEther Hi-Res Profile over AVTP."
Avoid anything that suggests Avnu blessing; the doc and the code paths should make it impossible for a user to think they're getting certified Milan behavior.

### Phase D — Interop honesty

**Goal:** Make it impossible for a user to deploy this and discover the hard way that it doesn't work with their commercial Milan gear.

- Receiver / talker `--help` for the relevant flags spells out "AOEther-only; will not bind to commercial Milan listeners."
- `recipe-milan-extended.md` opens with the constraint, before any setup steps.
- AVDECC entity model advertises a vendor-private subfield identifying AOEther as the talker; receivers refuse to bind to AOEther-extended streams from non-AOEther talkers (no point — the rate / DoP semantics are AOEther-private).
- A clear logging line on the talker at startup: "Extended profile active: stream is not Milan-baseline-interoperable."

### Phase E — Validation

**Without commercial gear (does not need physical hardware beyond two Linux boxes + a high-rate-capable USB DAC):**

1. AOEther talker on box A, receiver on box B, single-VLAN dumb gigabit switch, jumbo frames enabled.
2. **Phase A test 1:** `talker --transport avtp --rate 384000 --channels 8 --format s24le --avdecc` emits via AVDECC-bound listener; receiver plays through a 384 kHz-capable USB DAC (e.g., Topping E70, RME ADI-2, Denafrips Ares II). Confirm sine sweep / per-channel ID tone / 24-h soak.
3. **Phase A test 2:** same with `--rate 705600` / 8 ch — verify jumbo-frame path.
4. **Phase B test 1:** `talker --transport avtp --source dsf --file <stereo DSD64 .dsf> --dop` → receiver `--dop-demod` → ALSA `DSD_U8` to a DSD64-capable DAC.
5. **Phase B test 2:** same with DSD128 (.dsf) → 352.8 kHz DoP-AAF wire path → receiver passthrough → DoP-aware DAC unpacks.
6. **Phase B test 3:** multichannel DSD64 (.dff) → 176.4 kHz × N-channel DoP-AAF → 8-ch DoP-aware DAC (Exasound e62 / e68, Merging NADAC if available).

**With commercial Milan gear (negative interop tests, prove honesty):**

7. AOEther extended-profile talker → MOTU AVB or Apple CoreAudio AVB listener.
   Expected: AVDECC binding fails or the listener silently rejects the stream format.
   Document the failure mode in `recipe-milan-extended.md`.

## Critical files

**New:**

- `common/dop.{h,c}` — DoP modulator / demodulator.
- `docs/recipe-milan-extended.md` — operator recipe.
- `docs/proposal-milan-extended.md` — this file (delete or move to `docs/design.md` §M12 once accepted).

**Modified:**

- [`common/avtp.c`](common/avtp.c) and [`common/avtp.h`](common/avtp.h) — `avtp_aaf_nsr_from_hz()` returns NSR=0 for unknown rates; new helper `avtp_aaf_describe_extended_format(...)` for AVDECC descriptor encoding.
- [`talker/src/talker.c`](talker/src/talker.c) — `--dop` flag; high-rate path; reject DSD-without-DoP under AVTP.
- [`receiver/src/receiver.c`](receiver/src/receiver.c) — DoP marker detection; `--dop-demod` flag; high-rate ALSA configuration.
- `avdecc/...` — extend stream-format advertisement; AOEther-talker fingerprint subfield in stream descriptor.
- [`docs/wire-format.md`](wire-format.md) — new "Extended profile" subsection under Mode 2.
- [`docs/design.md`](design.md) — M12 milestone entry; cross-reference from M5 §"Documented constraints" and M6 §"DoP encoder" (currently `[open]`, this proposal subsumes that work).
- [`README.md`](README.md) — recipes index entry.

**Reusable existing pieces:**

- [`common/packet.h:20-23`](common/packet.h:20) — `AOE_FMT_DOP_DSD64..DSD512` already reserved.
- AOEther's existing DSD source plumbing (`audio_source_dsf.c`, `audio_source_dff.c`, `audio_source_dsdsilence`) — DoP modulator sits at the output edge.
- AVDECC stream-format advertisement code (M7 Phase B, in `avdecc/`) — extend rather than replace.
- M5 AAF emit / parse path — extend `avtp_aaf_nsr_from_hz()` rather than fork.

## Open questions for the design-discussion issue

1. **Naming.**
   "AOEther-Extended Milan" vs "AOEther Hi-Res Profile over AVTP" vs something else.
   The name appears in code, docs, and CLI help; it should make the AOEther-only nature obvious.
2. **Where does the actual rate live in the AVDECC stream descriptor when NSR=0?**
   Options: vendor-private subfield in the stream-format octets; a separate stream-info descriptor extension; or piggyback on `current_sampling_rate` and trust both ends to ignore the wire NSR.
   The cleanest is probably a vendor-private extension; needs a closer read of IEEE 1722.1-2021 §7.2.7 to pick the right slot.
3. **DSD via DoP versus DSD via vendor AVTP subtype.**
   IEEE 1722 reserves vendor-specific AVTP subtypes that could carry native DSD without the DoP wrapper.
   This would avoid DoP's 33% bandwidth overhead and reach DSD512 within practical rates.
   Cost: it's no longer AAF, so even AOEther-to-AOEther loses the "looks like Milan" property.
   Recommendation: stick with DoP for parity with Merging's approach and the "looks like Milan" story; document the vendor-subtype option as a future fallback if DoP overhead becomes painful at DSD256+ multichannel.
4. **Multi-stream bundling for high-rate multichannel.**
   384 kHz × 16-ch needs jumbo frames or a stream split.
   Milan has no analog of AES67's RFC 5888 `a=group:LS`.
   Options: (a) require jumbo frames everywhere, (b) emit N parallel AAF streams with a shared `stream_id` family + AOEther-private "this is part of bundle X, sub-stream Y" advertised in the AVDECC descriptor.
   Defer until Phase A validation actually hits the MTU wall in practice.
5. **Whether to ship at all.**
   If the only motivation is "consistent Milan transport everywhere," and Modes 1/3 already serve the high-rate / DSD case, the cost-benefit may not pencil out.
   Pre-implementation user demand check is appropriate.

## Out of scope

- **Avnu Alliance ratification.**
  Not pursuing.
  AOEther isn't large enough to drive standards-body work and the audiophile use case isn't in Avnu's roadmap.
- **Interop with Avid MTRX, MOTU AVB, Focusrite RedNet, L-Acoustics, Apple CoreAudio AVB.**
  These will not honor extended-profile streams.
  Phase D documents this; we don't try to make it work.
- **DSD512 over DoP.**
  Requires 1411.2 kHz PCM, beyond practical AAF rates and beyond what jumbo frames + Class A can carry comfortably at multichannel.
  DSD512 stays on Modes 1/3.
- **MSRP / SRP bandwidth reservations at extended rates.**
  AOEther doesn't implement MSRP yet (M7 Phase B Stream Reservation work is separate).
  When MSRP lands, extended-profile streams need higher bandwidth class allocations than baseline; tracked there.
- **gPTP-disciplined `avtp_timestamp` at extended rates.**
  M7 Phase B work; orthogonal to this proposal.

## Cost-benefit summary

**Engineering cost:** ~3–5 weekends.
- Phase A: 1–2 weekends (NSR=0 plumbing + AVDECC descriptor extension + receiver-side rate parsing).
- Phase B: 1–2 weekends (DoP modulator/demodulator + integration + USB DAC interop with DSD-aware DACs).
- Phase C/D/E: 1 weekend (docs + validation, modulo hardware availability).

**Code surface:** ~600–1000 lines across ~6 files.
Mostly extending existing M5 + M7 paths, not new infrastructure.

**Value delivered:** transport-consistency for users who want a Milan-everywhere story, including DSD and high-rate PCM, between AOEther endpoints.
No new capability vs Modes 1/3; only consistency.

**Recommended only if** there's an articulated user demand for "I want to use Milan as my single transport across all rates and DSD."
Otherwise, the honest recommendation is to keep Milan at 48/96/192 kHz PCM and route 384 kHz / DSD via Modes 1/3.
The `atmos-network-choice.md` decision doc already steers users this way.
