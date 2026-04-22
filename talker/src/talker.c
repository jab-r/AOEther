#include "audio_source.h"
#include "avtp.h"
#include "packet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

/* Stream parameters. Channels, rate, and sample format are all runtime-
 * configured from M6 on. The "rate" field generalizes to "samples or DSD-
 * bytes per second per channel" so the per-microframe payload math (rate /
 * 8000) works uniformly across PCM and native DSD. */
#define STREAM_ID             0x0001
#define PACKET_PERIOD_NS      125000L          /* 125 µs = 1 USB microframe */
#define MICROFRAMES_PER_MS    8

#define DEFAULT_CHANNELS      2
#define DEFAULT_RATE_HZ       48000

/* DSD byte rates per channel (DSD bit rate ÷ 8). */
#define DSD64_BYTE_RATE       352800       /* 2.8224 MHz / 8 */
#define DSD128_BYTE_RATE      705600
#define DSD256_BYTE_RATE      1411200
/* DSD512 (2.8224 MB/s/ch) exceeds the wire format's u8 payload_count (255)
 * and needs packet splitting — deferred to M8. */

/* Ethernet II data payload max (frame - eth header) for standard 1500 MTU. */
#define ETH_MTU_PAYLOAD       1500

/* IP/UDP transport default port (interim; IANA TBD per docs/wire-format.md). */
#define DEFAULT_UDP_PORT      8805

/* DSCP Expedited Forwarding = 46 (0xB8 when shifted into TOS byte). */
#define DSCP_EF_TOS           0xB8

enum transport_mode {
    TRANSPORT_L2 = 0,    /* raw Ethernet, AF_PACKET, EtherType 0x88B5/0x88B6 */
    TRANSPORT_IP = 1,    /* UDP over IPv4/v6, magic-byte disambiguation */
    TRANSPORT_AVTP = 2,  /* IEEE 1722 AVTP AAF, EtherType 0x22F0 (Milan) */
};

/* Safety clamp on feedback-derived rate: ±1000 ppm of nominal. */
#define RATE_CLAMP_PPM        1000.0

/* Talker reverts to nominal after this long without FEEDBACK. */
#define FEEDBACK_STALE_MS     5000

static int rate_supported(int hz)
{
    switch (hz) {
    case 44100: case 48000:
    case 88200: case 96000:
    case 176400: case 192000:
        return 1;
    default:
        return 0;
    }
}

/* Map --format string to (AoE format code, bytes_per_sample, effective rate
 * in samples-or-DSD-bytes per sec per channel). For PCM, the caller's
 * --rate Hz is passed through; for DSD the rate is overridden to the DSD
 * byte rate. Returns 0 on success, -1 if the name is unknown. */
struct stream_format {
    uint8_t  code;
    int      bytes_per_sample;
    int      rate_override;   /* >0 means override --rate; 0 means use --rate */
    int      is_dsd;          /* native DSD path */
    const char *name;
};

static int parse_format(const char *s, struct stream_format *f)
{
    if (!s) return -1;
    static const struct stream_format table[] = {
        { AOE_FMT_PCM_S24LE_3,    3, 0,                   0, "pcm"    },
        { AOE_FMT_NATIVE_DSD64,   1, DSD64_BYTE_RATE,     1, "dsd64"  },
        { AOE_FMT_NATIVE_DSD128,  1, DSD128_BYTE_RATE,    1, "dsd128" },
        { AOE_FMT_NATIVE_DSD256,  1, DSD256_BYTE_RATE,    1, "dsd256" },
        /* DSD512+ overflows the wire format's u8 payload_count field; it
         * lands in M8 alongside the packet-splitting work. */
    };
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
        if (strcmp(s, table[i].name) == 0) {
            *f = table[i];
            return 0;
        }
    }
    return -1;
}

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int parse_mac(const char *s, uint8_t mac[6])
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xff) return -1;
        mac[i] = (uint8_t)v[i];
    }
    return 0;
}

static int iface_lookup(int sock, const char *name, int *ifindex, uint8_t src_mac[6])
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0) {
        perror("SIOCGIFINDEX");
        return -1;
    }
    *ifindex = ifr.ifr_ifindex;
    if (ioctl(sock, SIOCGIFHWADDR, &ifr) < 0) {
        perror("SIOCGIFHWADDR");
        return -1;
    }
    memcpy(src_mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
}

static int64_t ts_diff_ms(struct timespec a, struct timespec b)
{
    return (int64_t)(a.tv_sec - b.tv_sec) * 1000 +
           ((int64_t)a.tv_nsec - (int64_t)b.tv_nsec) / 1000000;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s --iface IF [transport options] [stream options]\n"
        "\n"
        "Transport (pick one):\n"
        "  --transport l2               raw Ethernet, AOE wrapper, default\n"
        "    --dest-mac AA:BB:CC:DD:EE:FF    (required)\n"
        "  --transport ip               UDP over IPv4/v6 (Mode 3, M4)\n"
        "    --dest-ip X.Y.Z.W | v6:literal  (required)\n"
        "    --port N                        UDP port, default %d\n"
        "  --transport avtp             IEEE 1722 AAF, Milan interop (Mode 2, M5)\n"
        "    --dest-mac AA:BB:CC:DD:EE:FF    (required; unicast or AVTP multicast)\n"
        "\n"
        "Source:\n"
        "  --source testtone|wav|alsa|dsdsilence   default: testtone (pcm) /\n"
        "                                          dsdsilence when --format is DSD\n"
        "  --file   PATH                WAV file, required with --source wav\n"
        "  --capture hw:CARD=...        ALSA capture device, required with --source alsa\n"
        "\n"
        "Stream format:\n"
        "  --format  FMT                pcm | dsd64 | dsd128 | dsd256\n"
        "                               default pcm. DoP and DSD512+ are deferred\n"
        "                               (DSD512+ needs packet splitting → M8).\n"
        "                               AVTP transport carries pcm only.\n"
        "  --channels N                 channel count (1..64, default %d)\n"
        "  --rate    HZ                 44100|48000|88200|96000|176400|192000 (default %d)\n"
        "                               (ignored for native DSD — rate is implied by --format)\n"
        "\n"
        "PCM payload is s24le-3 (24-bit little-endian packed). Native DSD payload is\n"
        "raw DSD bits, MSB-first within each byte, interleaved by channel. Sources\n"
        "must match channels, rate, and format exactly — AOEther never resamples.\n"
        "For music playback, point --capture at one half of a snd-aloop pair\n"
        "and route Roon/UPnP/AirPlay/PipeWire at the other half; see\n"
        "docs/recipe-*.md.\n",
        prog, DEFAULT_UDP_PORT, DEFAULT_CHANNELS, DEFAULT_RATE_HZ);
}

int main(int argc, char **argv)
{
    const char *iface = NULL;
    const char *dest_mac_s = NULL;
    const char *dest_ip_s = NULL;
    const char *source = NULL;         /* default resolved below from --format */
    const char *wav_path = NULL;
    const char *capture_pcm = NULL;
    const char *format_s = "pcm";
    int channels = DEFAULT_CHANNELS;
    int rate_hz = DEFAULT_RATE_HZ;
    enum transport_mode transport = TRANSPORT_L2;
    int udp_port = DEFAULT_UDP_PORT;

    static const struct option opts[] = {
        { "iface",     required_argument, 0, 'i' },
        { "dest-mac",  required_argument, 0, 'd' },
        { "dest-ip",   required_argument, 0, 'I' },
        { "transport", required_argument, 0, 'T' },
        { "port",      required_argument, 0, 'P' },
        { "source",    required_argument, 0, 's' },
        { "file",      required_argument, 0, 'f' },
        { "capture",   required_argument, 0, 'c' },
        { "channels",  required_argument, 0, 'C' },
        { "rate",      required_argument, 0, 'r' },
        { "format",    required_argument, 0, 'F' },
        { "help",      no_argument,       0, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:d:I:T:P:s:f:c:C:r:F:h", opts, NULL)) != -1) {
        switch (c) {
        case 'i': iface = optarg; break;
        case 'd': dest_mac_s = optarg; break;
        case 'I': dest_ip_s = optarg; break;
        case 'T':
            if      (strcmp(optarg, "l2") == 0)   transport = TRANSPORT_L2;
            else if (strcmp(optarg, "ip") == 0)   transport = TRANSPORT_IP;
            else if (strcmp(optarg, "avtp") == 0) transport = TRANSPORT_AVTP;
            else { fprintf(stderr, "talker: --transport must be l2, ip, or avtp\n"); return 2; }
            break;
        case 'P': udp_port = atoi(optarg); break;
        case 's': source = optarg; break;
        case 'f': wav_path = optarg; break;
        case 'c': capture_pcm = optarg; break;
        case 'C': channels = atoi(optarg); break;
        case 'r': rate_hz = atoi(optarg); break;
        case 'F': format_s = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (!iface) {
        usage(argv[0]);
        return 2;
    }
    if ((transport == TRANSPORT_L2 || transport == TRANSPORT_AVTP) && !dest_mac_s) {
        fprintf(stderr, "talker: --dest-mac required for --transport %s\n",
                transport == TRANSPORT_L2 ? "l2" : "avtp");
        return 2;
    }
    if (transport == TRANSPORT_IP && !dest_ip_s) {
        fprintf(stderr, "talker: --dest-ip required for --transport ip\n");
        return 2;
    }
    if (udp_port < 1 || udp_port > 65535) {
        fprintf(stderr, "talker: --port out of range\n");
        return 2;
    }
    if (channels < 1 || channels > 64) {
        fprintf(stderr, "talker: --channels must be 1..64 (got %d)\n", channels);
        return 2;
    }

    struct stream_format fmt;
    if (parse_format(format_s, &fmt) < 0) {
        fprintf(stderr, "talker: unknown --format %s\n", format_s);
        return 2;
    }
    /* For PCM the user-supplied --rate applies; native DSD overrides. */
    if (!fmt.is_dsd) {
        if (!rate_supported(rate_hz)) {
            fprintf(stderr, "talker: unsupported --rate %d for PCM\n", rate_hz);
            return 2;
        }
    } else {
        rate_hz = fmt.rate_override;
    }
    if (transport == TRANSPORT_AVTP && fmt.is_dsd) {
        fprintf(stderr, "talker: AVTP AAF does not carry DSD; use --transport l2 or ip with --format dsd*\n");
        return 2;
    }
    const uint8_t format_code = fmt.code;
    const int bytes_per_sample = fmt.bytes_per_sample;
    const int is_dsd = fmt.is_dsd;

    /* Default source depends on format: testtone (PCM sine) or dsdsilence. */
    if (!source) {
        source = is_dsd ? "dsdsilence" : "testtone";
    } else if (is_dsd && strcmp(source, "dsdsilence") != 0) {
        fprintf(stderr, "talker: --source %s is PCM-only; native DSD requires --source dsdsilence (default)\n", source);
        return 2;
    } else if (!is_dsd && strcmp(source, "dsdsilence") == 0) {
        fprintf(stderr, "talker: --source dsdsilence requires --format dsd64|128|256|512\n");
        return 2;
    }

    /* MTU check: at worst we need nominal samples-per-microframe plus a
     * small drift margin, times channels × bytes. Plus eth (14) + protocol
     * header (16 for AOE, 24 for AVTP-AAF). */
    const size_t proto_hdr_len = (transport == TRANSPORT_AVTP) ? AVTP_HDR_LEN
                                                               : AOE_HDR_LEN;
    const double nominal_spm = (double)rate_hz / 1000.0 / MICROFRAMES_PER_MS;
    const int max_samples_per_packet = (int)(nominal_spm + 0.5) + 4;
    const size_t max_payload = (size_t)max_samples_per_packet * channels * bytes_per_sample;
    const size_t max_frame = sizeof(struct ether_header) + proto_hdr_len + max_payload;
    if (max_payload + proto_hdr_len > ETH_MTU_PAYLOAD) {
        fprintf(stderr,
                "talker: ch=%d rate=%d needs %zu-byte frames — exceeds 1500-byte MTU.\n"
                "  (Packet splitting for very-high-rate multichannel is deferred; try\n"
                "  fewer channels or a lower rate. Worst-case payload = %zu B.)\n",
                channels, rate_hz, max_frame, max_payload);
        return 2;
    }

    /* AVTP AAF only carries integer PCM rates from the standard nsr table.
     * Reject AOE rates that don't have an AAF code. */
    uint8_t avtp_nsr_code = 0;
    if (transport == TRANSPORT_AVTP) {
        if (avtp_aaf_nsr_from_hz(rate_hz, &avtp_nsr_code) < 0) {
            fprintf(stderr, "talker: AVTP AAF has no NSR code for rate %d\n", rate_hz);
            return 2;
        }
    }

    /* Destination resolution. Exactly one of (dest_mac) or (dest_ss) is
     * populated depending on transport. For IP, auto-detect v4/v6 and
     * unicast/multicast from the literal in --dest-ip. */
    uint8_t dest_mac[6];
    struct sockaddr_storage dest_ss;
    socklen_t dest_ss_len = 0;
    int dest_family = AF_UNSPEC;
    int dest_is_multicast = 0;
    memset(&dest_ss, 0, sizeof(dest_ss));

    if (transport == TRANSPORT_L2 || transport == TRANSPORT_AVTP) {
        if (parse_mac(dest_mac_s, dest_mac) < 0) {
            fprintf(stderr, "talker: bad --dest-mac\n");
            return 2;
        }
    } else {
        struct in_addr v4;
        struct in6_addr v6;
        if (inet_pton(AF_INET, dest_ip_s, &v4) == 1) {
            dest_family = AF_INET;
            struct sockaddr_in *sin = (struct sockaddr_in *)&dest_ss;
            sin->sin_family = AF_INET;
            sin->sin_port = htons((uint16_t)udp_port);
            sin->sin_addr = v4;
            dest_ss_len = sizeof(*sin);
            /* IPv4 multicast = 224.0.0.0/4 (first octet 224..239). */
            uint8_t first = ((uint8_t *)&v4)[0];
            dest_is_multicast = (first >= 224 && first <= 239);
        } else if (inet_pton(AF_INET6, dest_ip_s, &v6) == 1) {
            dest_family = AF_INET6;
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)&dest_ss;
            sin6->sin6_family = AF_INET6;
            sin6->sin6_port = htons((uint16_t)udp_port);
            sin6->sin6_addr = v6;
            dest_ss_len = sizeof(*sin6);
            /* IPv6 multicast = ff00::/8. */
            dest_is_multicast = (v6.s6_addr[0] == 0xff);
        } else {
            fprintf(stderr, "talker: --dest-ip %s is neither IPv4 nor IPv6\n", dest_ip_s);
            return 2;
        }
    }

    struct audio_source *src = NULL;
    if (strcmp(source, "testtone") == 0) {
        src = audio_source_test_open(channels, rate_hz, bytes_per_sample);
    } else if (strcmp(source, "wav") == 0) {
        if (!wav_path) {
            fprintf(stderr, "talker: --file required with --source wav\n");
            return 2;
        }
        src = audio_source_wav_open(wav_path);
        if (src && (src->channels != channels || src->rate != rate_hz)) {
            fprintf(stderr,
                    "talker: WAV file is ch=%d rate=%d; talker configured ch=%d rate=%d. "
                    "They must match (no resampling in AOEther).\n",
                    src->channels, src->rate, channels, rate_hz);
            src->close(src);
            return 2;
        }
    } else if (strcmp(source, "alsa") == 0) {
        if (!capture_pcm) {
            fprintf(stderr, "talker: --capture hw:... required with --source alsa\n");
            return 2;
        }
        src = audio_source_alsa_open(capture_pcm, channels, rate_hz);
    } else if (strcmp(source, "dsdsilence") == 0) {
        src = audio_source_dsd_silence_open(channels, rate_hz);
    } else {
        fprintf(stderr, "talker: unknown --source %s\n", source);
        return 2;
    }
    if (!src) return 1;

    /* Socket setup. L2 mode uses two AF_PACKET raw sockets (one per
     * EtherType). IP mode uses one AF_INET/AF_INET6 UDP socket for both
     * TX data and RX feedback, distinguished by magic byte (0xA0 vs 0xA1). */
    int data_sock = -1;
    int fb_sock = -1;      /* == data_sock in IP mode */
    int ifindex = 0;
    uint8_t src_mac[6] = {0};
    struct sockaddr_ll data_to_ll = {0};   /* used only in L2 */

    if (transport == TRANSPORT_L2 || transport == TRANSPORT_AVTP) {
        uint16_t data_etype = (transport == TRANSPORT_AVTP) ? AVTP_ETHERTYPE
                                                            : AOE_ETHERTYPE;
        data_sock = socket(AF_PACKET, SOCK_RAW, htons(data_etype));
        if (data_sock < 0) {
            perror("socket(AF_PACKET, SOCK_RAW) data");
            fprintf(stderr, "talker: raw sockets need CAP_NET_RAW (try sudo)\n");
            return 1;
        }
        if (iface_lookup(data_sock, iface, &ifindex, src_mac) < 0) return 1;

        data_to_ll.sll_family = AF_PACKET;
        data_to_ll.sll_protocol = htons(data_etype);
        data_to_ll.sll_ifindex = ifindex;
        data_to_ll.sll_halen = 6;
        memcpy(data_to_ll.sll_addr, dest_mac, 6);

        /* Mode C feedback always travels on AOE-C (0x88B6) regardless of
         * data transport — Milan listeners ignore unknown EtherTypes. */
        fb_sock = socket(AF_PACKET, SOCK_RAW, htons(AOE_C_ETHERTYPE));
        if (fb_sock < 0) {
            perror("socket(AF_PACKET, SOCK_RAW) feedback");
            return 1;
        }
        struct sockaddr_ll fb_bind = {
            .sll_family = AF_PACKET,
            .sll_protocol = htons(AOE_C_ETHERTYPE),
            .sll_ifindex = ifindex,
        };
        if (bind(fb_sock, (struct sockaddr *)&fb_bind, sizeof(fb_bind)) < 0) {
            perror("bind(fb_sock)");
            return 1;
        }
        int fl = fcntl(fb_sock, F_GETFL, 0);
        if (fl >= 0) fcntl(fb_sock, F_SETFL, fl | O_NONBLOCK);
    } else {
        /* IP/UDP mode. Single SOCK_DGRAM socket, bound to port 0 (kernel
         * picks local port) so the receiver can learn our (ip,port) from
         * the source address of our data datagrams and FEEDBACK returns
         * flow back on the same socket. */
        data_sock = socket(dest_family, SOCK_DGRAM, 0);
        if (data_sock < 0) {
            perror("socket(AF_INET*, SOCK_DGRAM)");
            return 1;
        }
        if (iface_lookup(data_sock, iface, &ifindex, src_mac) < 0) return 1;

        /* DSCP EF: encourages WMM AC_VO on WiFi, priority queueing on
         * DSCP-aware wired switches. Advisory. */
        int tos = DSCP_EF_TOS;
        if (dest_family == AF_INET) {
            if (setsockopt(data_sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) < 0) {
                perror("setsockopt(IP_TOS)");
                /* non-fatal */
            }
        } else {
            int tclass = DSCP_EF_TOS;
            if (setsockopt(data_sock, IPPROTO_IPV6, IPV6_TCLASS,
                           &tclass, sizeof(tclass)) < 0) {
                perror("setsockopt(IPV6_TCLASS)");
            }
        }

        if (dest_is_multicast) {
            /* Outgoing multicast interface + TTL + suppress loopback. */
            if (dest_family == AF_INET) {
                struct ip_mreqn mreq;
                memset(&mreq, 0, sizeof(mreq));
                mreq.imr_ifindex = ifindex;
                if (setsockopt(data_sock, IPPROTO_IP, IP_MULTICAST_IF,
                               &mreq, sizeof(mreq)) < 0) {
                    perror("setsockopt(IP_MULTICAST_IF)");
                }
                int ttl = 16;
                setsockopt(data_sock, IPPROTO_IP, IP_MULTICAST_TTL,
                           &ttl, sizeof(ttl));
                int loop = 0;
                setsockopt(data_sock, IPPROTO_IP, IP_MULTICAST_LOOP,
                           &loop, sizeof(loop));
            } else {
                if (setsockopt(data_sock, IPPROTO_IPV6, IPV6_MULTICAST_IF,
                               &ifindex, sizeof(ifindex)) < 0) {
                    perror("setsockopt(IPV6_MULTICAST_IF)");
                }
                int hops = 16;
                setsockopt(data_sock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
                           &hops, sizeof(hops));
                int loop = 0;
                setsockopt(data_sock, IPPROTO_IPV6, IPV6_MULTICAST_LOOP,
                           &loop, sizeof(loop));
            }
        }

        /* Bind to ephemeral local port. Needed so recvfrom() on this
         * socket picks up FEEDBACK replies. */
        if (dest_family == AF_INET) {
            struct sockaddr_in local = { .sin_family = AF_INET };
            local.sin_addr.s_addr = htonl(INADDR_ANY);
            local.sin_port = 0;
            if (bind(data_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
                perror("bind(udp v4)");
                return 1;
            }
        } else {
            struct sockaddr_in6 local = { .sin6_family = AF_INET6 };
            local.sin6_addr = in6addr_any;
            local.sin6_port = 0;
            if (bind(data_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
                perror("bind(udp v6)");
                return 1;
            }
        }

        int fl = fcntl(data_sock, F_GETFL, 0);
        if (fl >= 0) fcntl(data_sock, F_SETFL, fl | O_NONBLOCK);

        fb_sock = data_sock;   /* single-socket IP mode */
    }

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    if (tfd < 0) { perror("timerfd_create"); return 1; }
    struct itimerspec its = {
        .it_interval = { 0, PACKET_PERIOD_NS },
        .it_value    = { 0, PACKET_PERIOD_NS },
    };
    if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
        perror("timerfd_settime");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    uint8_t *frame = calloc(1, max_frame);
    if (!frame) return 1;

    /* L2 / AVTP have an Ethernet header prefix; IP mode does not. */
    if (transport == TRANSPORT_L2 || transport == TRANSPORT_AVTP) {
        struct ether_header *eth = (struct ether_header *)frame;
        memcpy(eth->ether_dhost, dest_mac, 6);
        memcpy(eth->ether_shost, src_mac, 6);
        eth->ether_type = htons(transport == TRANSPORT_AVTP
                                ? AVTP_ETHERTYPE : AOE_ETHERTYPE);
    }
    struct aoe_hdr      *aoe_hdr_p  = (struct aoe_hdr *)
        (frame + sizeof(struct ether_header));
    struct avtp_aaf_hdr *avtp_hdr_p = (struct avtp_aaf_hdr *)
        (frame + sizeof(struct ether_header));
    uint8_t *payload = frame + sizeof(struct ether_header) + proto_hdr_len;
    /* IP mode skips the 14-byte Ethernet-header prefix on the wire. */
    const size_t tx_offset = (transport == TRANSPORT_IP) ? sizeof(struct ether_header) : 0;

    /* AVTP stream_id convention: source MAC in high 6 bytes, our 16-bit
     * STREAM_ID in the low 2. Milan AVDECC normally allocates these via
     * the entity model; we mint a stable one ourselves until M7. */
    const uint64_t avtp_stream_id =
          ((uint64_t)src_mac[0] << 56)
        | ((uint64_t)src_mac[1] << 48)
        | ((uint64_t)src_mac[2] << 40)
        | ((uint64_t)src_mac[3] << 32)
        | ((uint64_t)src_mac[4] << 24)
        | ((uint64_t)src_mac[5] << 16)
        | (uint64_t)STREAM_ID;
    uint8_t avtp_seq8 = 0;

    if (transport == TRANSPORT_L2 || transport == TRANSPORT_AVTP) {
        const char *label = (transport == TRANSPORT_AVTP) ? "avtp" : "l2";
        fprintf(stderr,
                "talker: transport=%s iface=%s ifindex=%d\n"
                "        src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x\n"
                "        fmt=%s ch=%d rate=%d pps=8000 nominal_spp=%.2f max_spp=%d max_frame=%zuB feedback=on\n",
                label, iface, ifindex,
                src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
                dest_mac[0], dest_mac[1], dest_mac[2], dest_mac[3], dest_mac[4], dest_mac[5],
                transport == TRANSPORT_AVTP ? "AAF_INT24(BE)" : format_s,
                channels, rate_hz, nominal_spm, max_samples_per_packet, max_frame);
    } else {
        char ip_str[INET6_ADDRSTRLEN] = {0};
        if (dest_family == AF_INET) {
            inet_ntop(AF_INET,
                      &((struct sockaddr_in *)&dest_ss)->sin_addr,
                      ip_str, sizeof(ip_str));
        } else {
            inet_ntop(AF_INET6,
                      &((struct sockaddr_in6 *)&dest_ss)->sin6_addr,
                      ip_str, sizeof(ip_str));
        }
        fprintf(stderr,
                "talker: transport=ip dest=%s:%d family=%s %s\n"
                "        iface=%s ifindex=%d\n"
                "        fmt=%s ch=%d rate=%d pps=8000 nominal_spp=%.2f max_spp=%d max_payload=%zuB feedback=on\n",
                ip_str, udp_port,
                dest_family == AF_INET ? "v4" : "v6",
                dest_is_multicast ? "multicast" : "unicast",
                iface, ifindex,
                format_s, channels, rate_hz,
                nominal_spm, max_samples_per_packet,
                max_frame - sizeof(struct ether_header));
    }

    /* Mode C talker state: current target samples-per-microframe, fractional
     * accumulator. The effective rate is recomputed each tick from the
     * per-source feedback tracker below, so multi-receiver multicast picks
     * the slowest consumer to avoid xruns at any endpoint. */
    double samples_per_microframe = nominal_spm;
    double sample_accum = 0.0;

    /* Multi-source feedback tracking (IP mode). In L2 mode there's only
     * one receiver so slot 0 is used exclusively. */
#define MAX_FB_SOURCES 16
    struct fb_source {
        int in_use;
        struct sockaddr_storage addr;  /* IP mode: sender addr; L2: unused */
        socklen_t addrlen;
        uint32_t last_q;               /* Q16.16 samples/ms */
        uint16_t last_seq;
        int have_seq;
        struct timespec last_rx;
    } fb_src[MAX_FB_SOURCES] = {0};

    uint32_t seq = 0;
    uint64_t late_wakeups = 0;
    uint64_t fb_rx = 0, fb_ignored = 0;

    while (!g_stop) {
        /* 1. Drain any pending feedback frames (non-blocking). */
        for (;;) {
            uint8_t fb_buf[128];
            struct sockaddr_storage src_addr;
            socklen_t src_addrlen = sizeof(src_addr);
            ssize_t fn;
            if (transport == TRANSPORT_IP) {
                fn = recvfrom(fb_sock, fb_buf, sizeof(fb_buf), 0,
                              (struct sockaddr *)&src_addr, &src_addrlen);
            } else {
                fn = recv(fb_sock, fb_buf, sizeof(fb_buf), 0);
            }
            if (fn < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                perror("recv(fb)");
                break;
            }

            /* Locate the AoE-C header. L2 has the Ethernet header in front;
             * IP has just the UDP payload. */
            const struct aoe_c_hdr *fh;
            size_t hdr_off = (transport == TRANSPORT_IP) ? 0 : sizeof(struct ether_header);
            if ((size_t)fn < hdr_off + AOE_C_HDR_LEN) continue;
            fh = (const struct aoe_c_hdr *)(fb_buf + hdr_off);
            if (!aoe_c_hdr_valid(fh)) continue;
            if (fh->frame_type != AOE_C_TYPE_FEEDBACK) continue;
            if (ntohs(fh->stream_id) != STREAM_ID) continue;

            uint32_t q = ntohl(fh->value);
            double spms = (double)q / 65536.0;
            double new_spm = spms / (double)MICROFRAMES_PER_MS;

            double max_spm = nominal_spm * (1.0 + RATE_CLAMP_PPM * 1e-6);
            double min_spm = nominal_spm * (1.0 - RATE_CLAMP_PPM * 1e-6);
            if (new_spm < min_spm || new_spm > max_spm) {
                fb_ignored++;
                continue;
            }

            /* Find or allocate a slot for this source. In L2 mode, every
             * feedback comes from the single receiver — use slot 0. In IP
             * mode, match by family + addr + port. */
            int slot = -1;
            if (transport != TRANSPORT_IP) {
                /* L2 / AVTP: a single physical talker-receiver pair, slot 0. */
                slot = 0;
                fb_src[0].in_use = 1;
            } else {
                for (int i = 0; i < MAX_FB_SOURCES; i++) {
                    if (!fb_src[i].in_use) continue;
                    if (fb_src[i].addr.ss_family != src_addr.ss_family) continue;
                    int match = 0;
                    if (src_addr.ss_family == AF_INET) {
                        const struct sockaddr_in *a = (const struct sockaddr_in *)&fb_src[i].addr;
                        const struct sockaddr_in *b = (const struct sockaddr_in *)&src_addr;
                        match = (a->sin_port == b->sin_port) &&
                                (a->sin_addr.s_addr == b->sin_addr.s_addr);
                    } else {
                        const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)&fb_src[i].addr;
                        const struct sockaddr_in6 *b = (const struct sockaddr_in6 *)&src_addr;
                        match = (a->sin6_port == b->sin6_port) &&
                                (memcmp(&a->sin6_addr, &b->sin6_addr, 16) == 0);
                    }
                    if (match) { slot = i; break; }
                }
                if (slot < 0) {
                    for (int i = 0; i < MAX_FB_SOURCES; i++) {
                        if (!fb_src[i].in_use) {
                            slot = i;
                            fb_src[i].in_use = 1;
                            memcpy(&fb_src[i].addr, &src_addr, src_addrlen);
                            fb_src[i].addrlen = src_addrlen;
                            fb_src[i].have_seq = 0;
                            break;
                        }
                    }
                }
                if (slot < 0) {
                    /* Table full; drop. A real deployment would evict LRU;
                     * for M4 with MAX=16 this is a soft upper bound. */
                    fb_ignored++;
                    continue;
                }
            }

            /* Reject stale (out-of-order) feedback from the same source. */
            uint16_t s16 = ntohs(fh->sequence);
            if (fb_src[slot].have_seq) {
                int16_t d = (int16_t)(s16 - fb_src[slot].last_seq);
                if (d <= 0) { fb_ignored++; continue; }
            }
            fb_src[slot].last_seq = s16;
            fb_src[slot].have_seq = 1;
            fb_src[slot].last_q = q;
            clock_gettime(CLOCK_MONOTONIC, &fb_src[slot].last_rx);
            fb_rx++;
        }

        /* 2. Compute effective rate = min(q) across non-stale sources.
         * Taking the slowest keeps every receiver from xrunning. */
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        uint32_t min_q = 0;
        int have_any = 0;
        for (int i = 0; i < MAX_FB_SOURCES; i++) {
            if (!fb_src[i].in_use) continue;
            if (ts_diff_ms(now_ts, fb_src[i].last_rx) > FEEDBACK_STALE_MS) continue;
            if (!have_any || fb_src[i].last_q < min_q) {
                min_q = fb_src[i].last_q;
                have_any = 1;
            }
        }
        if (have_any) {
            double spms = (double)min_q / 65536.0;
            samples_per_microframe = spms / (double)MICROFRAMES_PER_MS;
        } else {
            samples_per_microframe = nominal_spm;
        }

        /* 3. Wait for timer tick(s). */
        uint64_t ticks;
        ssize_t r = read(tfd, &ticks, sizeof(ticks));
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("timerfd read");
            break;
        }
        if (ticks > 1) late_wakeups++;

        /* 4. Emit `ticks` packets. Each packet's payload_count is the
         *    integer part of the accumulator; residual carries forward. */
        for (uint64_t i = 0; i < ticks && !g_stop; i++) {
            sample_accum += samples_per_microframe;
            int pc = (int)sample_accum;
            if (pc < 1) pc = 1;
            if (pc > max_samples_per_packet) pc = max_samples_per_packet;
            sample_accum -= pc;

            if (src->read(src, payload, (size_t)pc) < 0) {
                g_stop = 1;
                break;
            }

            const size_t payload_bytes =
                (size_t)pc * channels * bytes_per_sample;
            size_t frame_len_total =
                sizeof(struct ether_header) + proto_hdr_len + payload_bytes;

            if (transport == TRANSPORT_AVTP) {
                /* AAF carries 24-bit samples big-endian; ALSA gives us LE.
                 * Swap in place before transmit. */
                avtp_swap24_inplace(payload,
                                    (size_t)pc * (size_t)channels);
                avtp_aaf_hdr_build(avtp_hdr_p,
                                   avtp_stream_id,
                                   avtp_seq8++,
                                   0,                    /* avtp_timestamp (no PTP yet) */
                                   AAF_FORMAT_INT24,
                                   avtp_nsr_code,
                                   (uint16_t)channels,
                                   24,                   /* bit_depth */
                                   (uint16_t)payload_bytes);
            } else {
                aoe_hdr_build(aoe_hdr_p, STREAM_ID, seq, 0,
                              (uint8_t)channels, format_code, (uint8_t)pc,
                              AOE_FLAG_LAST_IN_GROUP);
            }

            ssize_t sent;
            if (transport == TRANSPORT_IP) {
                sent = sendto(data_sock, frame + tx_offset, frame_len_total - tx_offset, 0,
                              (struct sockaddr *)&dest_ss, dest_ss_len);
            } else {
                sent = sendto(data_sock, frame, frame_len_total, 0,
                              (struct sockaddr *)&data_to_ll, sizeof(data_to_ll));
            }
            if (sent < 0) {
                if (errno == EINTR) break;
                perror("sendto");
                g_stop = 1;
                break;
            }
            seq++;
        }
    }

    fprintf(stderr,
            "talker: shutting down; sent=%u late_wakeups=%llu fb_rx=%llu fb_ignored=%llu\n",
            seq, (unsigned long long)late_wakeups,
            (unsigned long long)fb_rx, (unsigned long long)fb_ignored);

    src->close(src);
    close(tfd);
    if (fb_sock != data_sock) close(fb_sock);
    close(data_sock);
    free(frame);
    return 0;
}
