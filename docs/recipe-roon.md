# Recipe: Roon

Roon (roonlabs.com) is a widely used music management / streaming platform. Its network audio protocol, RAAT, is closed. AOEther bridges Roon by running **RoonBridge** (Roon's free endpoint binary) on the talker box, configuring it to output to an ALSA loopback, and capturing from the other side of the loopback.

Target: you have a Roon Core running somewhere on your network, and you want a new Roon endpoint that plays through AOEther to a remote USB DAC.

**Clock adaptation**: RoonBridge handles async sinks gracefully — it observes sink hardware rate via ALSA and paces accordingly. Expect glitch-free playback. Roon's own statistics will show clock-related events if any occur.

## One-time setup (talker box)

### 1. Load the ALSA loopback module

```sh
sudo modprobe snd-aloop
echo snd-aloop | sudo tee /etc/modules-load.d/snd-aloop.conf
aplay -l | grep Loopback   # confirm card present
```

### 2. Install RoonBridge

Download the Linux x64 installer from roonlabs.com and run it:

```sh
wget https://download.roonlabs.com/builds/RoonBridge_linuxx64.sh
chmod +x RoonBridge_linuxx64.sh
sudo ./RoonBridge_linuxx64.sh
```

It installs a systemd unit and starts on boot. Check it's running:

```sh
systemctl status roonbridge
```

(For ARM talkers e.g. an extra Pi, use `RoonBridge_linuxarmv7hf.sh` or `RoonBridge_linuxarmv8.sh` as appropriate.)

### 3. Configure RoonBridge to output to the loopback

In the Roon remote app (on phone or desktop), go to **Settings → Audio**. The talker box will show up as an available zone; under it, several ALSA devices are listed. Find the **Loopback PCM (hw:Loopback,0,0)** entry. Click **Enable**, give it a name like "AOEther Bridge," and under the gear icon set:

- **Device Format**: Custom → 48000 / 24-bit
- **Resync Delay**: default
- **Volume Control**: Fixed Volume (let the DAC handle volume)
- **Enable MQA Core Decoder**: off (M1 doesn't carry MQA)

## Per-session

With Roon configured and the loopback enabled, just run AOEther:

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

In the Roon app, select "AOEther Bridge" as the playback zone and hit play. Audio flows Roon Core → RoonBridge → snd-aloop → AOEther talker → Ethernet → AOEther receiver → USB DAC.

## Hi-res stereo and multichannel (M2)

To pass 192 kHz bit-exact, configure both the RoonBridge sink and the AOEther talker+receiver to 192 kHz:

1. In Roon → Settings → Audio → (gear on the Loopback sink), set **Device Format** to **Custom** → `192000` / `24-bit`.
2. Talker: `sudo ./talker/build/talker --source alsa --capture hw:Loopback,1,0 --channels 2 --rate 192000 ...`
3. Receiver: `sudo ./receiver/build/receiver --dac hw:CARD=...,DEV=0 --channels 2 --rate 192000 ...`

For multichannel (e.g., 5.1 Atmos bed or surround music), Roon can output multichannel to an ALSA device if the sink advertises it. Configure the Loopback device to match: the kernel's `snd-aloop` is format-agnostic so you just tell Roon to use e.g. `48000 / 24-bit / 6-channel` as the device format, then run talker+receiver with `--channels 6 --rate 48000`.

Roon will auto-downconvert sources that exceed the sink's capability (e.g., a 192 kHz track into a 48 kHz sink does lossy SRC inside Roon). For bit-exact playback run the sink at the highest rate you'll actually send.

## Caveats

- **DSD**: not carried until M6. RoonBridge will convert DSD to PCM when the sink is PCM-only.
- **Roon Ready certification**: we're not Roon Ready. We appear as a "Roon Bridge" endpoint, which is fully functional but not the marketed logo. Roon Ready requires NDA and a cert lab pass; post-M8 question if at all.
- **MTU**: Roon can theoretically send 12 ch × 384 kHz, but AOEther's M2 rejects that at startup (exceeds 1500-byte MTU). 12 ch × 192 kHz fits; see `docs/design.md` §M2.

## Troubleshooting

- **Endpoint not visible in Roon app**: check `systemctl status roonbridge`. Firewall may be blocking RAAT — by default RoonBridge discovery uses multicast + ports 9003/9100-9200 TCP.
- **Audio drops when skipping tracks**: Roon resyncs at track boundaries. AOEther should recover within ~1 s via the FEEDBACK loop's usual bounded-drift behavior; if drops are persistent, raise `--latency-us` on the receiver.
- **Clock drift messages in Roon's log**: expected to be rare. If frequent, check `aplay -D hw:Loopback,1,0 -vv` for buffer underruns on the capture side.
