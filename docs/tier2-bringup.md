# Tier 2 bring-up guide

This is the doc for bringing the AOEther receiver up on a Tier 2 Linux SBC — a board with hardware-PTP-capable Ethernet and enough peripherals to host a USB DAC. Tier 2 is what unlocks gPTP-disciplined emission (Mode B), sub-microsecond clock sync, and multi-receiver phase alignment per `docs/design.md` §M3.

Tier 1 (Raspberry Pi) works fine for single-receiver listening but can't do hardware PTP. Tier 2 closes that gap.

**Status:** This doc is ahead of the code. The existing receiver binary runs on any arm64 Debian/Ubuntu with `libasound2`, so the userspace side is straightforward. The harder pieces — hardware PTP integration, multi-receiver phase alignment — arrive in follow-up work once someone has the board in hand. See the "What still needs code" section at the bottom.

## Reference platform: NXP i.MX 8M Mini

Chosen as the reference because it has the cleanest Linux story for hardware PTP:

- **ENET_QOS** MAC with IEEE 1588v2 hardware timestamping built in.
- **Mainline kernel support** for the Ethernet MAC, clocks, and USB host — no vendor fork required, though the vendor BSP is often more polished for initial bring-up.
- **USB 2.0 HS host ports** exposed on most carrier boards. UAC2 DACs work with stock `snd_usb_audio`.
- **SoM + carrier** form factor from ~$150 (Kontron/Phytec/CompuLab/Variscite SoMs on evaluation carriers).

Any of the following should also work and are secondary targets once the i.MX 8M Mini path is proven:

- **NXP i.MX 8M Plus** SoM — same stack plus a second TSN-capable 1 GbE port; useful for M8 redundancy.
- **Marvell ClearFog Base** — `mvneta` Ethernet driver, hardware PTP.
- **TI AM6548 (BeagleBone AI-64)** — mainline support, community-friendly, hardware PTP via the `am65-cpsw` driver.

## Hardware checklist

- SoM with i.MX 8M Mini (Kontron pITX-SMARC-SXAL, Phytec phyBOARD-Polis, Variscite DART-MX8M-MINI on eval carrier, or NXP EVK).
- USB-A host port (most carriers break out at least one).
- Ethernet cable to a wired switch — **not** to WiFi, PTP over WiFi doesn't work.
- A USB DAC for playback.
- A Tier 1 receiver (Pi) and a talker on the same switch for soak testing.
- (Optional) A PPS output or scope-capable signal from both the Tier 1 and Tier 2 receivers' DACs for phase-alignment measurement at M3 endgame.

## OS choice

Two reasonable paths:

### Path A: Debian 12 (Bookworm) arm64

Fastest to a working receiver. Works on most commercial SoMs that ship with a Debian image or can boot a generic Debian arm64 image on top of the vendor's U-Boot + kernel.

```sh
# On the Tier 2 board, after booting Debian arm64:
sudo apt update
sudo apt install build-essential libasound2-dev linuxptp
git clone https://github.com/jab-r/AOEther.git
cd AOEther
make
```

Debian 12 ships a 6.1-series kernel with mainline i.MX 8M Mini Ethernet and PTP support. Confirm with:

```sh
uname -r      # expect 6.1.x or newer
ethtool -T eth0
# Look for: hardware-transmit (SOF_TIMESTAMPING_TX_HARDWARE)
#           hardware-receive  (SOF_TIMESTAMPING_RX_HARDWARE)
#           hardware-raw-clock (SOF_TIMESTAMPING_RAW_HARDWARE)
# If present, hardware PTP is available.
```

### Path B: Yocto / custom BSP

Better for integration into a product. Longer bring-up. Use NXP's `meta-imx` layers on top of a stock poky. Build a `core-image-base` with `linuxptp`, `alsa-utils`, `libasound`, and the AOEther receiver recipe (TBD). Kernel: whatever LTS the BSP tracks; 6.1 or newer recommended for best mainline-matching PTP behavior.

For M3 docs purposes, Path A is what you want. Path B is the right answer for a shippable product but it doesn't change what the receiver binary itself does.

## First boot checks

Before worrying about PTP, confirm the boring stuff works:

```sh
aplay -L | grep -A1 '^hw:'
# Your USB DAC should appear. If not, dmesg for snd_usb_audio errors.

ip link show eth0
ethtool eth0
# 1000 Mb/s full duplex on a wired switch.
```

Then run the receiver in M1/M2 mode (no PTP yet) to confirm the talker → Tier 2 → USB DAC path works:

```sh
sudo ./receiver/build/receiver \
    --iface eth0 \
    --dac hw:CARD=<your-dac>,DEV=0
```

On the talker elsewhere on the switch:

```sh
sudo ./talker/build/talker \
    --iface eno1 \
    --dest-mac <tier2-mac> \
    --source testtone
```

You should hear the 1 kHz tone. `fb_sent` on the receiver should be rising. This validates that everything except PTP works, which is 80% of the bring-up.

## Enabling hardware PTP

See [`ptp-setup.md`](ptp-setup.md) for the full ptp4l/phc2sys configuration. Summary:

1. Confirm `ethtool -T eth0` reports hardware timestamping support (above).
2. Configure `ptp4l` with a gPTP (802.1AS) profile on eth0 as slave.
3. Run `phc2sys` to discipline the system clock from the PHC.
4. Measure offset stability over ~60 s — should converge to sub-µs on a wired LAN.

Once that's in place, the AOEther receiver will be ready for the M3 code changes (see below) that consume `presentation_time`.

## What still needs code

This doc describes the prep. The code work below is what future M3 PRs will deliver once the board is running:

- **Talker:** add `--ptp-clock` / `--presentation-time-offset-ms` flags. When configured, read `CLOCK_TAI` (which `phc2sys` slaves to gPTP) at emission, stamp `presentation_time = low32(current_tai_ns + offset_ns)` in each data frame. Default behavior unchanged.
- **Receiver:** consume `presentation_time` when non-zero. Compute `target_tai = reconstruct_high_bits(presentation_time, current_tai_ns)`. Schedule `snd_pcm_writei()` such that samples reach the DAC approximately at `target_tai`. ALSA's scheduling is coarse; real accuracy is limited by ALSA period size, not by the wire-format timestamp.
- **Receiver:** statistics on presentation-time arrival jitter for debugging (log deltas between expected arrival time and actual, per second).
- **Multi-receiver phase alignment:** with two receivers both running `ptp4l` slave on the same gPTP master, and the talker stamping `presentation_time`, both receivers should present samples within a few µs of each other. Measure with a scope on the DAC analog outputs playing the same click track.

None of those land in this PR; they wait for hardware validation.

## Known deferred from M3

- WiFi timing. Mode B is wired-only; `ptp4l` over WiFi gets 100s of µs at best.
- Milan interop. Sub-µs sync on Tier 2 opens the door but Milan needs AVTP/AVDECC (M5 / M7).
- Redundancy (FRER-style). Post-M8 per Appendix C.
- Productized firmware image. Staying with Debian/Ubuntu for bring-up; Yocto-based shippable image is a separate effort.

## References

- [linuxptp project](https://linuxptp.sourceforge.net/) — `ptp4l`, `phc2sys`, `pmc`.
- NXP i.MX 8M Mini reference manual — ENET_QOS chapter for PTP registers and timestamp capture semantics.
- IEEE 802.1AS-2020 — gPTP profile.
- IEEE 1588-2019 — the broader PTPv2 spec.
- `Documentation/networking/timestamping.rst` in the Linux kernel tree — how `SO_TIMESTAMPING` surfaces hardware timestamps to userspace.
