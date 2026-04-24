# Recipe: UPnP / DLNA MediaRenderer

UPnP MediaRenderer is the open-standard protocol used by audiophile controllers like JRiver, BubbleUPnP (Android), Kazoo (Linn), mconnect (iOS), Foobar2000, and the controller side of MinimServer / Asset UPnP / BubbleUPnP Server.
AOEther bridges UPnP by running **upmpdcli + MPD** on the talker box: upmpdcli exposes a UPnP MediaRenderer to the network, MPD does the actual decode (including native DSD from DSF / DFF), and MPD writes the decoded stream into one half of an ALSA loopback that the AOEther talker captures from.

This is the audiophile-grade path.
It carries native DSD64 / DSD128 / DSD256 end-to-end with no DoP wrapping, no PCM intermediate, and no resampling — bits go from the DSF / DFF file on your library to the USB DAC's DSD input untouched.
A simpler PCM-only fallback (gmrender-resurrect, one apt install, no DSD) is documented at the end if your DAC isn't DSD-capable or you're only streaming Internet radio.

**Clock adaptation**: MPD writes at the file's nominal rate; the talker's ALSA capture edge absorbs the residual source-vs-DAC drift via hold-last-sample tail-repeat (see `design.md` §"Clock architecture").
At typical 20 ppm DAC drift this settles at roughly one held byte per second — for native DSD that's a single bit-cell repetition, well below any DAC's noise floor.
The capture ring depth is set by `--capture-buffer-ms` on the talker (default 100 ms); for DLNA / UPnP use we recommend **200 ms**, which buys ~170 minutes between hold events during transient drift.

## Prerequisites

- A DSD-capable USB DAC on the receiver, recognized natively by `snd_usb_audio` (see `recipe-dsd.md` §"Step 2" for the DAC → `--alsa-format` mapping table).
- Talker box on Linux with `snd-aloop` available, a working `aoether` build, and ports 1900/UDP and 49152–65535/TCP open on the LAN for SSDP / UPnP HTTP.
- The talker binary must include `--capture-format dsd_u8` support (added to AOEther alongside this recipe; rebuild from main if your binary is older).

## One-time setup (talker box)

### 1. Load the ALSA loopback module

```sh
sudo modprobe snd-aloop
echo snd-aloop | sudo tee /etc/modules-load.d/snd-aloop.conf
aplay -l | grep Loopback
```

### 2. Install MPD and upmpdcli

On Debian / Ubuntu:

```sh
sudo apt install mpd upmpdcli
```

upmpdcli is in Debian / Ubuntu repos from bookworm / 22.04 onwards; on older distros use the upstream package from `https://www.lesbonscomptes.com/upmpdcli/`.

### 3. Configure MPD for native DSD into the loopback

Edit `/etc/mpd.conf` and replace the default `audio_output` block with:

```
audio_output {
    type            "alsa"
    name            "AOEther Loopback"
    device          "hw:Loopback,0,0"
    dop             "no"
    auto_resample   "no"
    auto_format     "no"
    auto_channels   "no"
}
```

Key points:

- `dop "no"` disables DoP wrapping — we want raw DSD bits on the loopback, not 24-bit PCM marker frames.
- `auto_resample`, `auto_format`, `auto_channels` set to `"no"` make MPD refuse to silently convert sample rates / formats / channel counts; if a track doesn't match what the loopback is currently open at, MPD will fail playback rather than transcode.
  This is exactly what we want — AOEther performs no SRC anywhere in the path, so we'd rather see a clean failure than a silent quality regression.
- The `device` is the **playback** side of `snd-aloop` (subdevice 0); the talker captures from subdevice 1 of the same card.

Restart MPD:

```sh
sudo systemctl restart mpd
```

### 4. Configure upmpdcli

Edit `/etc/upmpdcli.conf`:

```
friendlyname = AOEther UPnP
mpdhost = localhost
mpdport = 6600
ownqueue = 1
upnpav = 1
```

Start it:

```sh
sudo systemctl enable --now upmpdcli
systemctl status upmpdcli
```

Confirm it's announcing itself on the LAN:

```sh
gssdp-discover -i eno1 -t urn:schemas-upnp-org:device:MediaRenderer:1
```

You should see "AOEther UPnP" with an HTTP description URL.

## Per-session — DSD playback

Pick one DSD rate per listening session (DSD64, DSD128, DSD256 …) and start the talker and receiver matched to that rate.
AOEther does not auto-renegotiate format on track change — see "Caveats" below.

```sh
# On the talker box — DSD64 example
sudo ./talker/build/talker \
    --iface eno1 \
    --dest-mac <pi-mac> \
    --source alsa \
    --capture hw:Loopback,1,0 \
    --capture-format dsd_u8 \
    --format dsd64 \
    --capture-buffer-ms 200

# On the Pi
sudo ./receiver/build/receiver \
    --iface eth0 \
    --dac hw:CARD=<your-dsd-dac>,DEV=0 \
    --format dsd64
```

For DSD128 / DSD256, replace `dsd64` on both sides with `dsd128` / `dsd256` and load that session's playlist in the controller.

In your UPnP controller (JRiver, BubbleUPnP, Kazoo, mconnect, …), select **AOEther UPnP** as the renderer.
Pick DSD tracks from a UPnP MediaServer (MinimServer is the standard; Plex, JRiver-as-server, Asset UPnP also work) and play.

The DAC's front-panel indicator should read "DSD64" (or "DSD128" / "DSD256") — not "PCM 176.4" (DoP) — confirming native DSD all the way through.

## Per-session — PCM playback

For PCM tracks (FLAC, WAV, ALAC, hi-res PCM up to whatever your library has), the same stack works — just start the talker in PCM mode and configure MPD's loopback open at the matching PCM rate:

```sh
sudo ./talker/build/talker \
    --iface eno1 \
    --dest-mac <pi-mac> \
    --source alsa \
    --capture hw:Loopback,1,0 \
    --capture-format pcm_s24_3le \
    --format pcm \
    --rate 96000 \
    --channels 2 \
    --capture-buffer-ms 200

sudo ./receiver/build/receiver \
    --iface eth0 \
    --dac hw:CARD=<your-dac>,DEV=0 \
    --format pcm \
    --rate 96000
```

Same single-format-per-session restriction.

## Caveats

- **One format per session**.
  AOEther locks the wire format at startup and snd-aloop's playback side is open at a single ALSA format / rate.
  A library that mixes DSD64, DSD128, and PCM rates needs a session restart on rate changes.
  This is consistent with AOEther's no-SRC, no-format-renegotiation discipline; if it bites in practice we'll add per-track format-change handling in a follow-up (open an issue).
- **Disable any server-side transcoding** in your MediaServer (MinimServer's "Stream transcode" tab, JRiver's "DLNA conversion" panel) — pin both DSD and PCM to "Original".
  Transcoding silently to FLAC or DoP defeats the bit-exact path.
- **No volume control in the loopback path**.
  Any volume scaling on the renderer side destroys DSD's marker invariants and resamples PCM with no dither.
  Set MPD's mixer to `none` (default in recent MPD) and keep the controller at unity / volume disabled.
  All level control happens at the DAC.
- **DSD512+ is M8**, not M6.
  AOEther's wire-format and talker / receiver code accept `--format dsd512`, `dsd1024`, `dsd2048`, but DSD512+ requires per-microframe packet splitting that lands in M8 alongside the DAC test matrix.
  For now, cap at DSD256.
- **MPD's `dop` setting matters**.
  If `dop "yes"` is left on, MPD will wrap DSD into 24-bit PCM markers, the loopback opens as 24-bit PCM, and the talker would need `--capture-format pcm_s24_3le` + `--format pcm --rate 176400` (or similar) — losing the bit-exact native-DSD path.
  Keep `dop "no"`.

## Troubleshooting

- **Renderer doesn't appear in controller**: firewall blocking SSDP multicast (port 1900 UDP) or upmpdcli's HTTP port (default 49152). Open these on the LAN.
- **Controller plays but DAC indicator says "PCM"**: MPD silently fell back to DoP or PCM. Check `journalctl -u mpd` — look for "ALSA does not support DSD" or similar. Verify `dop "no"` and `auto_format "no"` in `mpd.conf`. Verify the DAC's `snd_usb_audio` quirk entry advertises native DSD (see `recipe-dsd.md`).
- **MPD logs "Failed to open ALSA … device busy"**: PulseAudio or PipeWire grabbed the loopback. Stop or reconfigure them: `systemctl --user stop pipewire-pulse` (PipeWire) or `pulseaudio --kill` (PulseAudio), and add `Loopback` to their respective ignore lists.
- **MPD logs "Failed … unsupported format"**: the loopback playback side is locked to the previous session's format. Restart MPD between rate changes; the talker may also need restarting if its `--capture-format` / `--format` no longer matches.
- **Track changes within a rate cause clicks**: that's MPD's gapless-playback handling at format / channel boundaries, independent of AOEther. Pin all tracks in a session to the same rate / channel count, or accept the brief gap.

## PCM-only fallback: gmrender-resurrect

If you don't have a DSD-capable DAC and just want one-`apt install` PCM streaming (Internet radio, podcast playback, casual UPnP from a phone), gmrender-resurrect is a single-package alternative.
It's a GStreamer-based MediaRenderer that decodes everything to PCM, so it cannot carry native DSD — for that, use the upmpdcli + MPD path above.

```sh
sudo apt install gmediarender

# /etc/default/gmediarender:
# ENABLED=1
# DAEMON_USER="gmrender"
# UUID="<uuidgen>"
# FRIENDLY_NAME="AOEther UPnP (PCM)"
# INITIAL_VOLUME_DB=0
# ALSA_DEVICE=hw:Loopback,0,0

sudo systemctl enable --now gmediarender
```

Then run the talker as in the "Per-session — PCM playback" section above (`--capture-format pcm_s24_3le --format pcm`).

**License note**: both upmpdcli and MPD are GPL-2-or-later; gmrender-resurrect is GPL-2-only.
AOEther itself is GPL-3-or-later.
The processes only exchange audio samples over an ALSA loopback, so there's no linking relationship that would constrain either side.
