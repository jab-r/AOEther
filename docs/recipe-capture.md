# Recipe: system audio capture (PipeWire / PulseAudio)

Any desktop application — Tidal app, Spotify desktop, a browser tab playing Bandcamp, `mpv`, `mpd`, `cmus` — can feed AOEther as long as it plays through the OS audio stack. This recipe uses an `snd-aloop` kernel loopback plus PipeWire routing; it's the lowest-friction path because it doesn't require installing any extra daemons.

Target: you're sitting at a Linux machine, you want whatever you play to come out of the remote DAC.

**Clock adaptation**: PipeWire's per-node rate matching is adaptive. It observes the sink's actual hardware rate and paces smoothly. Expect glitch-free playback end to end.

## One-time setup (talker box)

### 1. Load the ALSA loopback kernel module

```sh
sudo modprobe snd-aloop

# Persist across reboots:
echo snd-aloop | sudo tee /etc/modules-load.d/snd-aloop.conf
```

Verify the Loopback card exists:

```sh
aplay -l | grep Loopback
# card N: Loopback [Loopback], device 0: Loopback PCM [Loopback PCM]
# card N: Loopback [Loopback], device 1: Loopback PCM [Loopback PCM]
```

Note the card number (N). The two "devices" (0 and 1) are the two halves of the loopback — audio written to `hw:Loopback,0,0` appears for capture at `hw:Loopback,1,0`, and vice versa.

### 2. Create a PipeWire sink that targets the loopback playback side

Drop this in `~/.config/pipewire/pipewire.conf.d/99-aoether.conf`:

```
context.modules = [
    { name = libpipewire-module-alsa-sink
        args = {
            node.name = "aoether"
            node.description = "AOEther (to remote DAC)"
            api.alsa.path = "hw:Loopback,0,0"
            audio.format = "S24_3LE"
            audio.rate = 48000
            audio.channels = 2
        }
    }
]
```

Reload PipeWire:

```sh
systemctl --user restart pipewire pipewire-pulse
```

`pw-cli list-objects | grep -A2 aoether` should show the new sink.

## Per-session (talker)

Route the app you want to AOEther. Either set it as default:

```sh
wpctl set-default $(wpctl status | awk '/aoether/ {print $2}' | tr -d '.')
```

…or per-stream in `pavucontrol` → Playback tab → redirect the stream to "AOEther (to remote DAC)".

Run the AOEther talker:

```sh
sudo ./talker/build/talker \
    --iface eno1 \
    --dest-mac <pi-mac> \
    --source alsa \
    --capture hw:Loopback,1,0
```

On the Pi (receiver), the usual:

```sh
sudo ./receiver/build/receiver \
    --iface eth0 \
    --dac hw:CARD=Dragonfly,DEV=0
```

Play something. You should hear it through the remote DAC.

## Caveats

- **Format lock**: the PipeWire sink is pinned to `S24_3LE @ 48000`. Apps running at 44.1 kHz (most non-hi-res streaming) will be resampled by PipeWire on its way into the sink. AOEther itself does no resampling; if you need bit-exact 44.1 kHz you'll need M2's rate negotiation.
- **Volume knob**: PipeWire's volume control on the aoether sink is digital attenuation before the loopback. For audiophile use keep it at 0 dB and control volume on the DAC. Setting it below 0 dB throws away bits.
- **Latency**: end-to-end is dominated by PipeWire's scheduling (~10–40 ms typical) plus the AOEther jitter buffer (5 ms default). Fine for music; not for video sync.

## Troubleshooting

- **No sink appears**: `journalctl --user -u pipewire` — look for ALSA sink load errors. A common cause is `snd-aloop` not loaded, or the wrong card number in the config (`hw:Loopback,0,0` assumes it's the first loopback; if another virtual card was registered first, it might be `hw:Loopback_1,0,0`).
- **Audio plays locally instead of through AOEther**: your default sink wasn't changed. `wpctl status` shows current default; `wpctl set-default ...` to change.
- **Underruns on the talker**: the source app is producing samples too slowly. Raise PipeWire's `node.quantum` for the aoether sink, or check for CPU contention.
