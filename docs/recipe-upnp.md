# Recipe: UPnP / DLNA MediaRenderer

UPnP MediaRenderer is the open-standard protocol used by controllers like BubbleUPnP (Android), Kazoo (Linn), mconnect (iOS), Foobar2000, JRiver, and many NAS/DLNA servers. AOEther bridges UPnP by running **gmrender-resurrect** (a minimal, mature MediaRenderer daemon) on the talker box, pointed at an ALSA loopback.

Target: you have a UPnP controller app and a UPnP media server (or just a controller that can push URLs), and you want the rendered output to play through AOEther to a remote USB DAC.

**Clock adaptation**: gmrender-resurrect is a naïve ALSA writer — it doesn't track sink rate and runs at nominal. Under Mode C's DAC-disciplined sink rate, expect the kernel loopback to push back on gmrender occasionally (< 1 event per hour at typical 20 ppm crystal spreads), which gmrender handles as a transient xrun and recovers from. Audible as a very occasional soft tick; fine for casual listening. If you're an audiophile who can't tolerate even that, use the Roon recipe instead — RoonBridge is adaptive.

**License note**: gmrender-resurrect is GPL2-licensed; you install it separately via apt. AOEther itself stays Apache 2.0.

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
    --capture hw:Loopback,1,0

# On the Pi
sudo ./receiver/build/receiver \
    --iface eth0 \
    --dac hw:CARD=Dragonfly,DEV=0
```

In your UPnP controller (BubbleUPnP, Kazoo, mconnect, etc.), select **AOEther UPnP** as the playback renderer. Pick tracks from a UPnP MediaServer (MinimServer, minidlna, Plex DLNA) and hit play.

## Caveats

- **Format lock**: gmrender-resurrect does not itself resample. The UPnP controller typically queries the renderer's supported formats and picks one the renderer advertises. With the `ALSA_DEVICE=hw:Loopback,0,0` config above, the kernel loopback will accept a range of formats but we've asked AOEther to capture 48 kHz 24-bit — other rates will fail to play cleanly. For 44.1 kHz sources, configure the controller to transcode or limit the library to 48 kHz content until M2.
- **Gapless playback**: gmrender-resurrect has known gaps at track boundaries on some versions. Not an AOEther problem; upgrade the package or consider rygel as an alternative.
- **Album art / now-playing**: the UPnP controller handles all of that; AOEther is just transport.

## Troubleshooting

- **Renderer doesn't appear in controller**: firewall blocking SSDP multicast (port 1900 UDP) or HTTP (default 49494 TCP). Open these or disable the firewall on the talker box for the LAN.
- **Controller shows renderer but playback fails**: `journalctl -u gmediarender` — a common error is "Audio device busy" (something else has the Loopback card; stop PulseAudio/PipeWire probes with `pulseaudio --kill` or point PipeWire away from Loopback) or "unsupported format" (your source is 44.1 kHz or DSD; see above).
- **rygel as an alternative**: `apt install rygel` and configure its renderer plugin. Also LGPL. Has richer format support but bigger surface area.
