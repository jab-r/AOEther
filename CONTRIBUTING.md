# Contributing to AOEther

Thanks for your interest. AOEther is early-stage and actively welcoming contributors — this is an unusually good moment to get involved because the architecture is fresh and there's real design work to do, not just ticket-closing.

## Ways to contribute

- **Try it out** on hardware you have and tell us what breaks. Bug reports from real DAC models are immediately valuable because `snd_usb_audio` quirks vary per-DAC.
- **Review the design doc.** [`docs/design.md`](docs/design.md) is the source of truth for what we're building and why. Constructive pushback on the design is welcome; open an issue with the section reference and your concern.
- **Implement a milestone.** Milestones M1 through M8 are scoped concretely enough that someone can pick one up. If you want to claim one, open an issue saying so — we'll coordinate to avoid duplicate work.
- **Add a DAC to the "tested hardware" list.** For M2 onward we need a growing catalog of DACs confirmed to work. Standard report format: DAC model, firmware version, confirmed rates/formats, any quirks.
- **Contribute to the wire format spec.** If you find ambiguities in [`docs/wire-format.md`](docs/wire-format.md), propose clarifications.

## How to report a bug

Open a GitHub issue with:

- What hardware you're running on (receiver, talker, DAC, network)
- What kernel version (Linux) or firmware version (MCU)
- What you expected vs. what happened
- Minimal reproduction steps
- Relevant log output (`dmesg`, `journalctl`, receiver/talker stdout)

For audio glitches, recording a short sample of the bad output helps enormously.

## Development environment

### Talker and Linux receiver (Tier 1/2)

- Any recent Linux distro with a mainline kernel 6.1 or newer.
- Build deps: `build-essential`, `libasound2-dev`, `linuxptp` (from M3), `libnl-3-dev` (for later TSN config).
- CI runs on Ubuntu 22.04 and 24.04.

### MCU receiver (Tier 3, M6+)

- NXP MIMXRT1170-EVKB development board.
- NXP MCUXpresso IDE or command-line `arm-none-eabi-gcc` + Make.
- J-Link or DAP-Link debug probe.

### Testing

M1 deliverables include functional, timing, and soak tests documented in [`docs/design.md`](docs/design.md). PRs that add features should include test coverage; PRs that fix bugs should add a regression test where feasible.

## Code style

- **C:** K&R brace style, 4-space indent (not tabs), snake_case for function and variable names, SCREAMING_SNAKE_CASE for macros. Header guards via `#pragma once`. No trailing whitespace. Keep functions short; split files when a module exceeds ~500 lines.
- **Shell:** `#!/bin/sh` with POSIX-compatible syntax where possible. Quote all variable expansions. `set -eu`.
- **Markdown:** One sentence per line in source files (makes diffs reviewable). Fenced code blocks with language tags. Prefer prose over bulleted lists when explaining design decisions.

A `.clang-format` file and a `.editorconfig` will be added once M1 ships. Until then, match the existing style.

## Pull request flow

1. Open an issue first for anything non-trivial (new feature, architectural change, performance optimization). Small bug fixes can go directly to PR.
2. Fork and branch from `main`. Branch naming: `feature/<short-description>` or `fix/<short-description>`.
3. Commit early and often on your branch; we'll squash-merge if that keeps history clean, or rebase-merge if the commits have independent value.
4. PR description: explain the what and why, link to the issue, summarize how you tested it. Screenshots or audio samples where relevant.
5. CI must be green.
6. One reviewer approval merges.

## Commit messages

Imperative mood, 72-char subject line, optional body separated by a blank line. Reference issues by number where relevant.

```
Add DAC capability discovery to receiver

Parses /proc/asound/cardN/stream0 on startup and emits a
capability JSON blob over the control plane. Closes #42.
```

## Design discussion

For anything that changes the protocol, the wire format, or the milestone plan: open an issue labeled `design-discussion`. We'll work through the tradeoffs in the issue thread and update [`docs/design.md`](docs/design.md) when we converge. The design doc is the canonical record; keep it current.

## Code of conduct

Be kind. Assume good faith. Disagreements about technical direction are fine and expected; disagreements about people are not. If something feels off, reach out to the maintainers privately.

## License

By contributing you agree your contributions are licensed under the project's [Apache 2.0 License](LICENSE).
