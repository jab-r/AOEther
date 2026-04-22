# Recipe: mDNS-SD discovery

**M7 Phase A.** Receivers advertise themselves on the local network via mDNS-SD so talkers (and operators) don't need to hardcode MAC addresses or IP addresses.

Discovery is a convenience layer, never part of the data path.
The receiver still works fine with `--announce` off; conversely, the talker can still be driven from a static `--dest-mac` / `--dest-ip` even when the receiver is advertising.
Phase B (AVDECC for Milan controllers like Hive) is tracked separately — mDNS-SD here is the simple-home-network path.

## What the receiver publishes

When run with `--announce`, the receiver registers one service:

- **Service type:** `_aoether._udp`
- **Instance name:** `--name NAME` if given, else the machine's hostname. Avahi auto-appends " #2", " #3" on name collisions.
- **Port:** the `--port N` value (8805 default). Meaningful for `--transport ip`; still published as a reference for L2/AVTP even though those transports are MAC-addressed.
- **TXT records:**

| Key | Value | Example | Notes |
|---|---|---|---|
| `ver` | protocol version | `1` | Bumped on incompatible TXT-schema changes. |
| `role` | `receiver` or `talker` | `receiver` | Future-proofing; Phase A only publishes `receiver`. |
| `transport` | `l2`, `ip`, `avtp` | `ip` | Transport this receiver is listening on. |
| `format` | `pcm`, `dsd64`, … | `pcm` | Matches `--format`. |
| `channels` | integer | `2` | Matches `--channels`. |
| `rate` | Hz or DSD byte rate | `48000` | Matches effective rate (DSD byte rate for DSD). |
| `iface` | Linux iface name | `eth0` | Purely informational. |
| `dac` | ALSA PCM name | `hw:CARD=D90,DEV=0` | Purely informational. |
| `port` | UDP port | `8805` | Duplicate of SRV port; convenient for clients that only read TXT. |

Multiple capabilities per receiver (e.g. "supports PCM up to 192 kHz **and** DSD256") are not expressed yet — each run publishes one concrete configuration.
Phase B / AVDECC expresses the full capability matrix properly.

## Bring-up

### On the receiver

```sh
sudo apt install avahi-daemon libavahi-client-dev
sudo systemctl enable --now avahi-daemon
cd receiver && make

sudo ./build/receiver --iface eth0 \
                      --dac hw:CARD=Dragonfly,DEV=0 \
                      --transport ip --port 8805 \
                      --announce --name "living-room-dac"
```

The banner shows the published name:

```
mdns: published _aoether._udp as "living-room-dac"
receiver: transport=ip family=v4 unicast port=8805
          iface=eth0 dac=hw:CARD=Dragonfly,DEV=0 fmt=pcm ch=2 rate=48000 latency_us=5000 feedback=on
```

### From the talker box

Browse with the bundled helper:

```sh
cd tools && make
./build/aoether-browse
```

Typical output:

```
living-room-dac                   192.168.1.42:8805  host=pi5.local
    ver=1
    role=receiver
    transport=ip
    format=pcm
    channels=2
    rate=48000
    iface=eth0
    dac=hw:CARD=Dragonfly,DEV=0
    port=8805
```

Or use the distro tools directly — `avahi-browse` is installed with `avahi-utils` on most distros, and `dns-sd` is built into macOS:

```sh
# Linux:
avahi-browse -r -t _aoether._udp

# macOS:
dns-sd -B _aoether._udp
dns-sd -L "living-room-dac" _aoether._udp
```

### Feeding discovery into the talker

The talker doesn't yet auto-select discovered receivers (that's a small follow-up — likely a `--dest=mdns:NAME` shorthand).
For now, pipe the browse output into the invocation manually, or script it:

```sh
addr=$(./tools/build/aoether-browse --timeout 2 | awk '/living-room-dac/ { print $2 }')
ip=${addr%:*}
sudo ./talker/build/talker --iface eno1 \
                           --transport ip --dest-ip "$ip" \
                           --source testtone
```

## Interaction with Milan / AVTP (Phase B)

`_aoether._udp` is the AOEther-native service type, used regardless of transport.
Milan controllers (Hive, L-Acoustics controller, etc.) do not discover via mDNS-SD — they use AVDECC over L2.
Phase B will add an AVDECC entity responder so Hive sees AOEther receivers without any mDNS-SD machinery.
`--announce` stays useful even after Phase B lands: it's the right discovery surface for Mode 3 (IP/UDP) deployments and for any non-Milan client.

## Troubleshooting

**"mdns: not compiled in"** — rebuild after `apt install libavahi-client-dev`; the Makefile detects the library via `pkg-config` and falls back to a no-op stub otherwise.

**"mdns: avahi_client_new failed: Daemon not running"** — `systemctl start avahi-daemon` on the receiver box.

**`avahi-browse` shows nothing but the receiver says "published"** — avahi needs to reach the querying host on UDP 5353. Check firewalls, and remember mDNS doesn't cross subnets without an mDNS reflector.

**TXT record values with commas / spaces** — none of Phase A's keys need escaping. If we ever add comma-joined lists (e.g. `rates=44100,48000,96000`) they are valid DNS-TXT payload as-is, but be careful with shell quoting when scripting against them.

**Multiple receivers with the same DAC name** — Avahi appends " #2" automatically. If you want stable names, pass `--name` explicitly on each box.
