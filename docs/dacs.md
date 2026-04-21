# Tested DAC matrix

Per-DAC test reports covering the AOEther M2 configuration space: channel counts, sample rates, and observed quirks. This file grows as contributors test new hardware. A DAC need not appear here to work — the `snd_usb_audio` kernel stack supports a vast range of UAC2 devices — but this list is the ground truth for "known to work in AOEther."

## How to report

Open a PR adding a row (or a subsection if the DAC has interesting quirks) with:

- DAC model and firmware version (where exposed).
- Linux kernel version tested on (receiver side).
- Configurations that worked: `channels × rate × sample format` triples.
- Any `snd_usb_audio` quirks you had to add or disable (reference the `quirks-table.h` entry if one exists).
- Any xrun-frequency observations under Mode C soak.

Keep entries terse. One line per confirmed configuration.

## Stereo DACs (confirmed 2-channel through AOEther)

| DAC | Rates confirmed | Notes |
|-----|-----------------|-------|
| _(add your DAC here)_ | | |

## Multichannel DACs (6+ channel playback confirmed)

| DAC | Channels × rate | Notes |
|-----|-----------------|-------|
| _(add your DAC here)_ | | |

## Known problematic

DACs that appear in `lsusb` as UAC2 but fail in specific AOEther configurations. This is useful to document even when not a fix target for us — contributors trying the same DAC shouldn't hit the wall again.

| DAC | What fails | Workaround (if any) |
|-----|-----------|---------------------|
| _(add your DAC here)_ | | |

## Reference test sequence

Before filing a report, run through these to get a complete picture:

1. Receiver and talker both at `--channels 2 --rate 48000`: baseline, must play cleanly.
2. Receiver and talker at the DAC's max stereo rate (e.g., `--rate 192000`): confirms hi-res stereo path.
3. If the DAC is multichannel: receiver and talker at the DAC's native channel count × 48 kHz.
4. 1-hour Mode C soak at whatever config you're reporting; log `fb_sent` on the receiver and `underruns` on both sides.
5. Negative control: receiver with `--no-feedback`; confirms your feedback loop is doing real work (stream should drift and xrun within minutes).

## What's deliberately out of scope in M2

- 24-bit is the only format; 16-bit and 32-bit widths arrive later. Report hardware failures only for the 24-bit path.
- DSD support is M6.
- Multichannel at 384 / 705.6 / 768 kHz is not supported by M2's MTU check (packet splitting is deferred). Very-high-rate tests are for stereo only.
