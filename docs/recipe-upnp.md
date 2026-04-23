# Recipe: UPnP / DLNA MediaRenderer

UPnP MediaRenderer is the open-standard protocol used by controllers like BubbleUPnP (Android), Kazoo (Linn), mconnect (iOS), Foobar2000, JRiver, and many NAS/DLNA servers. AOEther bridges UPnP by running **gmrender-resurrect** (a minimal, mature MediaRenderer daemon) on the talker box, pointed at an ALSA loopback.

Target: you have a UPnP controller app and a UPnP media server (or just a controller that can push URLs), and you want the rendered output to play through AOEther to a remote USB DAC.

**Clock adaptation**: gmrender-resurrect writes at nominal rate, so it does not participate in Mode C's rate-match directly. The talker's ALSA capture edge absorbs the residual source-vs-DAC drift via hold-last-sample tail-repeat: when the capture ring runs dry, the last-read frame is repeated into the gap. At typical 20 ppm DAC drift this settles at roughly one repeated sample per second — ~20 µs plateaus, never a transient, inaudible on program material. The capture ring depth is set by `--capture-buffer-ms` on the talker (default 100 ms). For DLNA / UPnP use we recommend **`--capture-buffer-ms 200`**, which buys ~170 minutes between hold events during transient drift and makes the steady-state artifact rate indistinguishable from bit-exact. See `docs/design.md` §"Clock architecture" for the full framing.

**License note**: gmrender-resurrect is GPL-2-licensed and a separate runtime dependency installed via apt. AOEther itself is GPL-3-or-later — the two processes only exchange audio samples over an ALSA loopback, so there's no linking relationship that would constrain either side.

## One-time setup (talker box)

### 1. Load the ALSA loopback module

```sh
sudo modprobe snd-aloop
echo snd-aloop | sudo tee /etc/modules-load.d/snd-aloop.conf
aplay -l | grep Loopback
```

### 2. Install gmrender-resurrect

On Debian/Ubuntu:

```sh
sudo apt install gmediarender
```

(The package name is `gmediarender` in Debian; the binary is `gmediarender`.)

### 3. Configure it to output to the loopback

Edit `/etc/default/gmediarender`:

```
ENABLED=1
DAEMON_USER="gmrender"
UUID="<generate a uuid with `uuidgen`>"
FRIENDLY_NAME="AOEther UPnP"
INITIAL_VOLUME_DB=0
ALSA_DEVICE=hw:Loopback,0,0
```

Start it:

```sh
sudo systemctl enable --now gmediarender
systemctl status gmediarender
```

Confirm it's announcing itself on the LAN:

```sh
gssdp-discover -i eno1 -t urn:schemas-upnp-org:device:MediaRenderer:1
# Should list "AOEther UPnP" with an HTTP description URL.
```

## Per-session

```sh
# On the talker box
sudo ./talker/build/talker \
    --iface eno1 \
    --dest-mac <pi-mac> \
    --source alsa \
    --capture hw:Loopback,1,0 \
    --capture-buffer-ms 200

# On the Pi
sudo ./receiver/build/receiver \
    --iface eth0 \
    --dac hw:CARD=Dragonfly,DEV=0
```

In your UPnP controller (BubbleUPnP, Kazoo, mconnect, etc.), select **AOEther UPnP** as the playback renderer. Pick tracks from a UPnP MediaServer (MinimServer, minidlna, Plex DLNA) and hit play.

## Hi-res stereo and multichannel (M2)

gmrender-resurrect itself is format-agnostic (it forwards whatever the controller sends to the configured ALSA device). To run a different rate / channel count, start the AOEther talker and receiver with matching `--rate` and `--channels` values. The UPnP controller's transcoding settings determine what actually reaches gmrender; configure it to not transcode so source bits pass through to AOEther.

Multichannel UPnP content is less common in the wild than Roon multichannel; if your MediaServer has multichannel FLAC you can play it through with `--channels 6` (or whatever the source has) and an appropriate multichannel DAC on the receiver.

## Caveats

- **Format lock**: gmrender-resurrect does not itself resample, but the UPnP controller may. Disable transcoding in the controller for bit-exact playback. AOEther rejects any format mismatch at `snd_pcm_set_params()` on the capture side.
- **Gapless playback**: gmrender-resurrect has known gaps at track boundaries on some versions. Not an AOEther problem; upgrade the package or consider rygel as an alternative.
- **Album art / now-playing**: the UPnP controller handles all of that; AOEther is just transport.

## Troubleshooting

- **Renderer doesn't appear in controller**: firewall blocking SSDP multicast (port 1900 UDP) or HTTP (default 49494 TCP). Open these or disable the firewall on the talker box for the LAN.
- **Controller shows renderer but playback fails**: `journalctl -u gmediarender` — a common error is "Audio device busy" (something else has the Loopback card; stop PulseAudio/PipeWire probes with `pulseaudio --kill` or point PipeWire away from Loopback) or "unsupported format" (your source is 44.1 kHz or DSD; see above).
- **rygel as an alternative**: `apt install rygel` and configure its renderer plugin. Also LGPL. Has richer format support but bigger surface area.
