#include "audio_source.h"
#include "avdecc.h"
#include "avtp.h"
#include "packet.h"
#include "rtp.h"
#include "sap.h"
#include "sdp.h"
#include "ptp_pmc.h"

#include <pthread.h>

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
 * bytes per second per channel" so the per-packet payload math works
 * uniformly across PCM and native DSD.
 *
 * Modes 1/2/3 run at 8000 pps (125 µs per packet, one USB microframe);
 * Mode 4 (RTP/AES67) runs at PTIME — default 1 ms (1000 pps) per AES67,
 * optional 125 µs via --ptime. PACKET_PERIOD_DEFAULT_NS captures the
 * Modes-1-3 default; the runtime-resolved `packet_period_ns` below is
 * used everywhere else. */
#define STREAM_ID                    0x0001
#define PACKET_PERIOD_DEFAULT_NS     125000L
#define PACKET_PERIOD_AES67_1MS_NS   1000000L

#define DEFAULT_CHANNELS      2
#define DEFAULT_RATE_HZ       48000

/* DSD byte rates per channel (DSD bit rate ÷ 8). */
#define DSD64_BYTE_RATE       352800       /* 2.8224 MHz / 8 */
#define DSD128_BYTE_RATE      705600
#define DSD256_BYTE_RATE      1411200
#define DSD512_BYTE_RATE      2822400       /* M8: enabled by packet splitting. */
#define DSD1024_BYTE_RATE     5644800       /* Wire: ~3 fragments/microframe stereo. */
#define DSD2048_BYTE_RATE     11289600      /* Wire: ~6 fragments/microframe stereo. */

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
    TRANSPORT_RTP = 3,   /* RTP/AES67 over UDP (Mode 4, M9 Phase A) */
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
        { AOE_FMT_NATIVE_DSD512,  1, DSD512_BYTE_RATE,    1, "dsd512" },
        { AOE_FMT_NATIVE_DSD1024, 1, DSD1024_BYTE_RATE,   1, "dsd1024" },
        { AOE_FMT_NATIVE_DSD2048, 1, DSD2048_BYTE_RATE,   1, "dsd2048" },
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

/* AVDECC bind state. An AVDECC controller (Hive) issuing CONNECT_TX on
 * this talker fires on_bind from la_avdecc's executor thread; the
 * main send loop picks up the announced destination MAC on each
 * packet and overrides the CLI --dest-mac until DISCONNECT_TX arrives. */
static pthread_mutex_t g_avdecc_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t         g_avdecc_dest_mac[6];
static int             g_avdecc_dest_valid;

static void avdecc_on_bind(const uint8_t mac[6], uint64_t stream_id, void *user)
{
    (void)stream_id; (void)user;
    pthread_mutex_lock(&g_avdecc_mu);
    memcpy(g_avdecc_dest_mac, mac, 6);
    g_avdecc_dest_valid = 1;
    pthread_mutex_unlock(&g_avdecc_mu);
}

static void avdecc_on_unbind(void *user)
{
    (void)user;
    pthread_mutex_lock(&g_avdecc_mu);
    g_avdecc_dest_valid = 0;
    pthread_mutex_unlock(&g_avdecc_mu);
}

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

/* Fetch the primary IPv4 address bound to `name` via SIOCGIFADDR. Used to
 * populate the o= / a=source-filter fields of the outgoing SDP. Returns 0
 * and writes the address (in network byte order) on success; returns -1
 * if the interface has no IPv4 address (e.g. pure-L2 bring-up). */
static int iface_ipv4(const char *name, uint32_t *out_be)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    ifr.ifr_addr.sa_family = AF_INET;
    int rc = ioctl(s, SIOCGIFADDR, &ifr);
    close(s);
    if (rc < 0) return -1;
    const struct sockaddr_in *sin = (const struct sockaddr_in *)&ifr.ifr_addr;
    *out_be = sin->sin_addr.s_addr;
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
        "  --transport rtp              RTP/AES67 over UDP (Mode 4, M9)\n"
        "    --dest-ip X.Y.Z.W | v6:literal  (required; 239.X.X.X typical)\n"
        "    --port N                        UDP port, default 5004 (AES67)\n"
        "    --ptime US                      packet time: 1000 (AES67 default)\n"
        "                                    or 125 (low-latency profile)\n"
        "    --announce-sap                  emit SAP announcements on\n"
        "                                    239.255.255.255:9875 every 30 s\n"
        "                                    so Dante Controller / ANEMAN /\n"
        "                                    aes67-linux-daemon discover us\n"
        "                                    (M9 Phase B)\n"
        "    --session-name NAME             session name surfaced in SDP / SAP\n"
        "                                    (default: hostname + stream id)\n"
        "    --ptp                           use CLOCK_TAI for the RTP timestamp\n"
        "                                    base and advertise the PTPv2\n"
        "                                    grandmaster in SDP (M9 Phase C).\n"
        "                                    Requires ptp4l + phc2sys running —\n"
        "                                    see docs/ptp-setup.md. Reads the\n"
        "                                    current gmid via `pmc` every 30s;\n"
        "                                    falls back to :traceable if pmc\n"
        "                                    isn't installed or ptp4l is down.\n"
        "    --ptp-domain N                  PTP domain number (0..127, default 0)\n"
        "                                    advertised in the ts-refclk line\n"
        "    --sdp-only                      print the generated SDP to stdout\n"
        "                                    and exit; use when a controller\n"
        "                                    wants a static session file\n"
        "\n"
        "Source:\n"
        "  --source testtone|wav|alsa|dsdsilence|dsf\n"
        "                               default: testtone (pcm) /\n"
        "                               dsdsilence when --format is DSD\n"
        "  --file   PATH                WAV file for --source wav,\n"
        "                               DSF file for --source dsf\n"
        "  --capture hw:CARD=...        ALSA capture device, required with --source alsa\n"
        "\n"
        "Stream format:\n"
        "  --format  FMT                pcm | dsd64 | dsd128 | dsd256\n"
        "                             | dsd512 | dsd1024 | dsd2048\n"
        "                               default pcm. DSD512+ uses per-microframe\n"
        "                               packet splitting (wire-format.md §Cadence).\n"
        "                               DoP remains deferred.\n"
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
        "docs/recipe-*.md.\n"
        "\n"
        "Discovery / control plane:\n"
        "  --avdecc               start an AVDECC talker entity so Hive and other\n"
        "                         Milan controllers can discover this stream (M7 Phase B;\n"
        "                         needs la_avdecc submodule built — see docs/recipe-avdecc.md)\n"
        "  --name NAME            entity name for --avdecc (default: hostname + \"-talker\")\n",
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
    int ptime_us = 0;                 /* 0 = transport-default; user may set 125 or 1000 */
    int avdecc_enabled = 0;
    const char *entity_name = NULL;
    int announce_sap = 0;
    int ptp_enabled = 0;
    int sdp_only = 0;
    int ptp_domain = 0;
    const char *session_name = NULL;

    static const struct option opts[] = {
        { "iface",     required_argument, 0, 'i' },
        { "dest-mac",  required_argument, 0, 'd' },
        { "dest-ip",   required_argument, 0, 'I' },
        { "transport", required_argument, 0, 'T' },
        { "port",      required_argument, 0, 'P' },
        { "ptime",     required_argument, 0, 1001 },
        { "source",    required_argument, 0, 's' },
        { "file",      required_argument, 0, 'f' },
        { "capture",   required_argument, 0, 'c' },
        { "channels",  required_argument, 0, 'C' },
        { "rate",      required_argument, 0, 'r' },
        { "format",    required_argument, 0, 'F' },
        { "avdecc",    no_argument,       0, 'V' },
        { "name",      required_argument, 0, 'N' },
        { "announce-sap",  no_argument,       0, 1002 },
        { "session-name",  required_argument, 0, 1003 },
        { "ptp",           no_argument,       0, 1004 },
        { "sdp-only",      no_argument,       0, 1005 },
        { "ptp-domain",    required_argument, 0, 1006 },
        { "help",      no_argument,       0, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:d:I:T:P:s:f:c:C:r:F:VN:h", opts, NULL)) != -1) {
        switch (c) {
        case 'i': iface = optarg; break;
        case 'd': dest_mac_s = optarg; break;
        case 'I': dest_ip_s = optarg; break;
        case 'T':
            if      (strcmp(optarg, "l2") == 0)   transport = TRANSPORT_L2;
            else if (strcmp(optarg, "ip") == 0)   transport = TRANSPORT_IP;
            else if (strcmp(optarg, "avtp") == 0) transport = TRANSPORT_AVTP;
            else if (strcmp(optarg, "rtp") == 0)  transport = TRANSPORT_RTP;
            else { fprintf(stderr, "talker: --transport must be l2, ip, avtp, or rtp\n"); return 2; }
            break;
        case 'P': udp_port = atoi(optarg); break;
        case 1001: ptime_us = atoi(optarg); break;
        case 's': source = optarg; break;
        case 'f': wav_path = optarg; break;
        case 'c': capture_pcm = optarg; break;
        case 'C': channels = atoi(optarg); break;
        case 'r': rate_hz = atoi(optarg); break;
        case 'F': format_s = optarg; break;
        case 'V': avdecc_enabled = 1; break;
        case 'N': entity_name = optarg; break;
        case 1002: announce_sap = 1; break;
        case 1003: session_name = optarg; break;
        case 1004: ptp_enabled = 1; break;
        case 1005: sdp_only = 1; break;
        case 1006:
            ptp_domain = atoi(optarg);
            if (ptp_domain < 0 || ptp_domain > 127) {
                fprintf(stderr, "talker: --ptp-domain must be 0..127\n");
                return 2;
            }
            break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (!iface) {
        usage(argv[0]);
        return 2;
    }
    if ((transport == TRANSPORT_L2 || transport == TRANSPORT_AVTP) &&
        !dest_mac_s && !avdecc_enabled) {
        fprintf(stderr,
                "talker: --dest-mac required for --transport %s "
                "(or use --avdecc and let Hive CONNECT_TX bind at runtime)\n",
                transport == TRANSPORT_L2 ? "l2" : "avtp");
        return 2;
    }
    if ((transport == TRANSPORT_IP || transport == TRANSPORT_RTP) && !dest_ip_s) {
        fprintf(stderr, "talker: --dest-ip required for --transport %s\n",
                transport == TRANSPORT_IP ? "ip" : "rtp");
        return 2;
    }
    if ((announce_sap || ptp_enabled || sdp_only) && transport != TRANSPORT_RTP) {
        fprintf(stderr,
                "talker: --announce-sap / --ptp / --sdp-only require "
                "--transport rtp\n");
        return 2;
    }
    if (transport == TRANSPORT_RTP && udp_port == DEFAULT_UDP_PORT) {
        /* AES67's registered RTP port is 5004; retarget the default when
         * the user hasn't set --port explicitly. */
        udp_port = RTP_DEFAULT_PORT;
    }
    if (udp_port < 1 || udp_port > 65535) {
        fprintf(stderr, "talker: --port out of range\n");
        return 2;
    }

    /* Packet cadence. Modes 1/2/3 run at USB-microframe cadence (125 µs).
     * Mode 4 (RTP/AES67) uses PTIME — default 1 ms, optional 125 µs for
     * the low-latency AES67 profile. Other PTIME values (250 µs, 333 µs)
     * exist in the spec but aren't supported here yet. */
    long packet_period_ns;
    if (transport == TRANSPORT_RTP) {
        if (ptime_us == 0)                         ptime_us = RTP_PTIME_US_1MS;
        if (ptime_us != RTP_PTIME_US_1MS &&
            ptime_us != RTP_PTIME_US_125US) {
            fprintf(stderr,
                    "talker: --ptime %d unsupported for RTP "
                    "(use 1000 for AES67 default or 125 for low-latency)\n",
                    ptime_us);
            return 2;
        }
        packet_period_ns = (long)ptime_us * 1000L;
    } else {
        if (ptime_us != 0 && ptime_us != 125) {
            fprintf(stderr,
                    "talker: --ptime only applies to --transport rtp "
                    "(modes 1/2/3 are locked to 125 µs microframe cadence)\n");
            return 2;
        }
        packet_period_ns = PACKET_PERIOD_DEFAULT_NS;
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
    if (transport == TRANSPORT_RTP && fmt.is_dsd) {
        fprintf(stderr, "talker: AES67 RTP is PCM-only; use --transport l2 or ip with --format dsd*\n");
        return 2;
    }
    const uint8_t format_code = fmt.code;
    const int bytes_per_sample = fmt.bytes_per_sample;
    const int is_dsd = fmt.is_dsd;

    /* M9 Phase B + C — build one canonical SDP for the current stream.
     * Used by --sdp-only (print and exit) and --announce-sap (periodic
     * multicast announcement in the main loop below). */
    char sdp_text[SDP_MAX_LEN];
    int  sdp_len = 0;
    uint32_t origin_ipv4_be = 0;
    struct sdp_params sdp = {0};
    if (transport == TRANSPORT_RTP && (announce_sap || sdp_only)) {
        if (iface_ipv4(iface, &origin_ipv4_be) < 0) {
            fprintf(stderr,
                    "talker: could not read IPv4 address of %s — "
                    "--announce-sap / --sdp-only need an IPv4-assigned "
                    "interface (set one with `ip addr add`, then retry)\n",
                    iface);
            return 2;
        }
        char origin_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &origin_ipv4_be, origin_ip, sizeof origin_ip);

        sdp.origin_ip     = origin_ip;
        sdp.dest_ip       = dest_ip_s;
        sdp.port          = (uint16_t)udp_port;
        sdp.ttl           = 32;
        sdp.session_id    = (uint64_t)time(NULL);
        sdp.session_version = sdp.session_id;
        sdp.encoding      = SDP_ENC_L24;
        sdp.sample_rate_hz = (uint32_t)rate_hz;
        sdp.channels      = (uint8_t)channels;
        sdp.payload_type  = RTP_DEFAULT_PT_L24;
        sdp.ptime_us      = (uint32_t)ptime_us;
        sdp.refclk        = ptp_enabled ? SDP_REFCLK_PTP_TRACEABLE
                                        : SDP_REFCLK_NONE;
        sdp.ptp_domain    = (uint8_t)ptp_domain;
        if (ptp_enabled) {
            uint8_t gmid_bytes[8];
            if (ptp_pmc_read_gmid(gmid_bytes) == 0) {
                ptp_gmid_to_str(gmid_bytes, sdp.gmid_str,
                                sizeof sdp.gmid_str);
                sdp.refclk = SDP_REFCLK_PTP_GMID;
            }
        }
        if (session_name) {
            strncpy(sdp.session_name, session_name, SDP_MAX_SESSION_NAME - 1);
            sdp.session_name[SDP_MAX_SESSION_NAME - 1] = '\0';
        } else {
            char host[64] = "aoether";
            gethostname(host, sizeof host - 1);
            snprintf(sdp.session_name, SDP_MAX_SESSION_NAME,
                     "%s %uch/%u", host,
                     (unsigned)channels, (unsigned)rate_hz);
        }

        sdp_len = sdp_build(sdp_text, sizeof sdp_text, &sdp);
        if (sdp_len < 0) {
            fprintf(stderr, "talker: SDP build failed\n");
            return 1;
        }

        if (sdp_only) {
            fwrite(sdp_text, 1, (size_t)sdp_len, stdout);
            return 0;
        }
    }

    /* Default source depends on format: testtone (PCM sine) or dsdsilence.
     * --source dsf is the other DSD-capable source; everything else is PCM. */
    const int source_is_dsd = source && (strcmp(source, "dsdsilence") == 0 ||
                                         strcmp(source, "dsf") == 0);
    if (!source) {
        source = is_dsd ? "dsdsilence" : "testtone";
    } else if (is_dsd && !source_is_dsd) {
        fprintf(stderr,
                "talker: --source %s is PCM-only; native DSD requires "
                "--source dsdsilence (default) or --source dsf\n", source);
        return 2;
    } else if (!is_dsd && source_is_dsd) {
        fprintf(stderr,
                "talker: --source %s requires --format dsd64|dsd128|dsd256|dsd512|dsd1024|dsd2048\n",
                source);
        return 2;
    }

    /* MTU sizing and fragmentation parameters.
     *
     * AOE paths (Mode 1 / Mode 3) support per-microframe fragmentation: a
     * single microframe is split into K packets when either payload_count
     * (u8) or the MTU would otherwise overflow. See docs/wire-format.md
     * §"Cadence and fragmentation".
     *
     * AVTP AAF (Mode 2) and RTP/AES67 (Mode 4) do not fragment — splitting
     * across multiple frames per packet interval breaks interop with strict
     * Milan / AES67 listeners respectively. Those modes reject MTU-
     * overflowing configurations at startup. */
    const size_t proto_hdr_len =
        (transport == TRANSPORT_AVTP) ? AVTP_HDR_LEN :
        (transport == TRANSPORT_RTP)  ? RTP_HDR_LEN  :
                                        AOE_HDR_LEN;
    /* nominal_spm is now "samples per packet" in whatever cadence the
     * transport uses. Legacy name kept to minimize diff churn. */
    const double nominal_spm =
        (double)rate_hz * (double)packet_period_ns / 1e9;
    const int max_samples_per_microframe = (int)(nominal_spm + 0.5) + 4;
    const size_t max_microframe_payload =
        (size_t)max_samples_per_microframe * channels * bytes_per_sample;

    /* Per-fragment upper bound on payload_count: clamped by the u8 field
     * and by the worst-case per-fragment MTU budget. Fragmentation is an
     * AOE-only path (Modes 1 / 3). */
    int max_frag_pc = 0;
    if (transport != TRANSPORT_AVTP && transport != TRANSPORT_RTP) {
        const size_t per_ch_unit = (size_t)channels * bytes_per_sample;
        if (per_ch_unit == 0) {
            fprintf(stderr, "talker: invalid per-channel unit\n");
            return 2;
        }
        size_t mtu_budget_pc =
            (ETH_MTU_PAYLOAD - proto_hdr_len) / per_ch_unit;
        if (mtu_budget_pc > 255) mtu_budget_pc = 255;
        if (mtu_budget_pc < 1) {
            fprintf(stderr,
                    "talker: ch=%d bps=%d — a single sample/byte per channel "
                    "does not fit the 1500 B MTU (payload unit %zu B).\n",
                    channels, bytes_per_sample, per_ch_unit);
            return 2;
        }
        max_frag_pc = (int)mtu_budget_pc;
    }

    /* Startup MTU check for non-fragmenting transports. */
    if ((transport == TRANSPORT_AVTP || transport == TRANSPORT_RTP) &&
        max_microframe_payload + proto_hdr_len > ETH_MTU_PAYLOAD) {
        const char *mode_name =
            transport == TRANSPORT_AVTP ? "AVTP AAF" : "RTP/AES67";
        fprintf(stderr,
                "talker: ch=%d rate=%d under %s needs %zu-byte payload "
                "— exceeds 1500-byte MTU.\n"
                "  This transport does not support per-packet fragmentation "
                "(would break listener interop). Try --transport l2 or "
                "--transport ip, or reduce channels / rate.\n",
                channels, rate_hz, mode_name,
                max_microframe_payload + proto_hdr_len);
        return 2;
    }

    /* Per-frame buffer sizing:
     *   - `frame` holds one transmission unit = Ethernet header + proto
     *     header + one fragment's payload (for AOE) or one full packet
     *     (for AVTP / RTP).
     *   - `audio_buf` holds one full packet-interval's audio bytes, read
     *     from the source in one `read()` call and then sliced into
     *     fragments for AOE. AVTP / RTP read into this too and copy once
     *     into `frame`. */
    const size_t max_frag_payload =
        (transport == TRANSPORT_AVTP || transport == TRANSPORT_RTP)
            ? max_microframe_payload
            : ((size_t)max_frag_pc * channels * bytes_per_sample);
    const size_t max_frame = sizeof(struct ether_header) + proto_hdr_len + max_frag_payload;

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
        if (dest_mac_s) {
            if (parse_mac(dest_mac_s, dest_mac) < 0) {
                fprintf(stderr, "talker: bad --dest-mac\n");
                return 2;
            }
        }
        /* else: --avdecc is set; dest_mac stays 00:00:00:00:00:00 until
         * Hive CONNECT_TX binds us via the avdecc_on_bind callback.
         * The per-packet egress path swaps in the ACMP MAC before each
         * send, so the zero placeholder never reaches the wire. */
    } else {
        /* IP (Mode 3) and RTP (Mode 4) both take a dest IP. */
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
    } else if (strcmp(source, "dsf") == 0) {
        if (!wav_path) {
            fprintf(stderr, "talker: --file PATH.dsf required with --source dsf\n");
            return 2;
        }
        src = audio_source_dsf_open(wav_path);
        if (src && (src->channels != channels || src->rate != rate_hz)) {
            fprintf(stderr,
                    "talker: DSF file is ch=%d rate=%d (DSD bytes/s/ch); "
                    "talker configured ch=%d rate=%d. They must match — "
                    "use --format dsd64|dsd128|dsd256|dsd512|dsd1024|dsd2048 matching the file, "
                    "and --channels to match.\n",
                    src->channels, src->rate, channels, rate_hz);
            src->close(src);
            return 2;
        }
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
        .it_interval = { 0, packet_period_ns },
        .it_value    = { 0, packet_period_ns },
    };
    if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
        perror("timerfd_settime");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* AVDECC entity (M7 Phase B). Talker publishes a STREAM_OUTPUT so
     * Milan controllers can discover and bind it. AVDECC is L2/AVTP-only;
     * it has no meaning under RTP/AES67 (Ravenna uses SAP, not AVDECC). */
    if (avdecc_enabled && transport == TRANSPORT_RTP) {
        fprintf(stderr,
                "talker: --avdecc has no effect under --transport rtp "
                "(AES67 uses SAP/SDP for discovery; AVDECC is Milan-only)\n");
        avdecc_enabled = 0;
    }
    struct aoether_avdecc *avdecc = NULL;
    if (avdecc_enabled) {
        struct aoether_avdecc_config cfg = {
            .role        = AOETHER_AVDECC_TALKER,
            .entity_name = entity_name,
            .iface       = iface,
            .channels    = channels,
            .rate_hz     = rate_hz,
            .format_name = fmt.name,
        };
        avdecc = aoether_avdecc_open(&cfg, avdecc_on_bind, avdecc_on_unbind, NULL);
        if (!avdecc) {
            fprintf(stderr,
                    "talker: AVDECC entity open failed (continuing without --avdecc)\n");
        }
    }

    uint8_t *frame = calloc(1, max_frame);
    uint8_t *audio_buf = calloc(1, max_microframe_payload);
    if (!frame || !audio_buf) {
        fprintf(stderr, "talker: out of memory allocating frame/audio buffers\n");
        return 1;
    }

    /* L2 / AVTP have an Ethernet header prefix; IP and RTP do not
     * (kernel adds IP/UDP when sendto'ing on a DGRAM socket). */
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
    struct rtp_hdr      *rtp_hdr_p  = (struct rtp_hdr *)
        (frame + sizeof(struct ether_header));
    uint8_t *payload = frame + sizeof(struct ether_header) + proto_hdr_len;
    /* IP and RTP skip the 14-byte Ethernet-header prefix on the wire. */
    const size_t tx_offset =
        (transport == TRANSPORT_IP || transport == TRANSPORT_RTP)
            ? sizeof(struct ether_header) : 0;

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

    /* RTP/AES67 state: SSRC is a stable 32-bit random-ish identifier.
     * Mint it from the low 4 bytes of our MAC XOR with our STREAM_ID so
     * it survives talker restarts with the same config. Controllers use
     * SSRC to dedupe sources on the same (mcast, port) tuple. */
    const uint32_t rtp_ssrc =
        (((uint32_t)src_mac[2] << 24) |
         ((uint32_t)src_mac[3] << 16) |
         ((uint32_t)src_mac[4] <<  8) |
          (uint32_t)src_mac[5])
        ^ (uint32_t)STREAM_ID;
    /* RTP timestamp advances by samples-per-packet in the stream's media
     * clock ticks. Starts at a random-ish value to avoid collisions and
     * to match AES67's expectation that timestamps reflect media-clock
     * state rather than wall-clock zero.
     *
     * Without --ptp the base is CLOCK_MONOTONIC (M9 Phase A). With --ptp
     * the base is CLOCK_TAI, which linuxptp's phc2sys slews against the
     * PTPv2 grandmaster. That matches the AES67 expectation that the
     * RTP timestamp counts seconds from the PTP epoch on the media
     * clock (M9 Phase C). */
    struct timespec rtp_start_ts;
    clockid_t rtp_clock = ptp_enabled ? CLOCK_TAI : CLOCK_MONOTONIC;
    clock_gettime(rtp_clock, &rtp_start_ts);
    uint32_t rtp_timestamp = (uint32_t)(
        (uint64_t)rtp_start_ts.tv_sec * (uint64_t)rate_hz +
        ((uint64_t)rtp_start_ts.tv_nsec * (uint64_t)rate_hz) / 1000000000ull);
    uint16_t rtp_seq16 = 0;

    /* M9 Phase B — SAP announcer state. Opened once, used to periodically
     * emit the SDP built above on 239.255.255.255:9875 so AES67
     * controllers can discover this talker. Sending is driven from the
     * main loop (every SAP_ANNOUNCE_INTERVAL_S seconds); one deletion
     * packet is emitted on shutdown so controllers drop the session
     * promptly instead of waiting for the session-timeout. */
    int sap_fd = -1;
    struct sockaddr_in sap_to = {0};
    uint16_t sap_msg_id = 0;
    struct timespec last_sap_ts = {0};
    uint8_t sap_pkt[SDP_MAX_LEN + SAP_HDR_LEN + SAP_MIME_LEN];
    int sap_pkt_len = 0;
    if (announce_sap) {
        sap_fd = sap_open_tx_socket(iface);
        if (sap_fd < 0) {
            fprintf(stderr,
                    "talker: sap_open_tx_socket failed; continuing without "
                    "SAP announcements\n");
        } else {
            sap_to.sin_family = AF_INET;
            sap_to.sin_port   = htons(SAP_PORT);
            inet_pton(AF_INET, SAP_IPV4_ADDR_STR, &sap_to.sin_addr);
            /* RFC 2974 msg-id hash: any stable 16-bit value for this
             * session. XOR low halves of session_id to get one. */
            sap_msg_id = (uint16_t)(sdp.session_id ^ (sdp.session_id >> 16));

            sap_pkt_len = sap_build(sap_pkt, sizeof sap_pkt,
                                    SAP_ANNOUNCE,
                                    origin_ipv4_be, sap_msg_id,
                                    sdp_text, (size_t)sdp_len);
            if (sap_pkt_len > 0) {
                ssize_t n = sendto(sap_fd, sap_pkt, (size_t)sap_pkt_len, 0,
                                   (struct sockaddr *)&sap_to, sizeof sap_to);
                if (n < 0) perror("sendto(SAP announce)");
            }
            clock_gettime(CLOCK_MONOTONIC, &last_sap_ts);
        }
    }

    /* Predicted fragment count for the nominal microframe. K=1 is the
     * common case; larger values report the split that will be applied
     * to DSD512+ and high-rate multichannel PCM. AVTP/RTP report 0
     * (they never fragment). */
    int nominal_K = 1;
    if (transport != TRANSPORT_AVTP && transport != TRANSPORT_RTP &&
        max_frag_pc > 0) {
        int nominal_pc = (int)(nominal_spm + 0.5);
        if (nominal_pc < 1) nominal_pc = 1;
        nominal_K = (nominal_pc + max_frag_pc - 1) / max_frag_pc;
    }

    const double pps = 1e9 / (double)packet_period_ns;
    const int feedback_enabled_for_transport =
        (transport != TRANSPORT_RTP);

    if (transport == TRANSPORT_L2 || transport == TRANSPORT_AVTP) {
        const char *label = (transport == TRANSPORT_AVTP) ? "avtp" : "l2";
        fprintf(stderr,
                "talker: transport=%s iface=%s ifindex=%d\n"
                "        src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x\n"
                "        fmt=%s ch=%d rate=%d pps=%.0f nominal_spp=%.2f max_frag_pc=%d frags/packet=%d max_frame=%zuB feedback=on\n",
                label, iface, ifindex,
                src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
                dest_mac[0], dest_mac[1], dest_mac[2], dest_mac[3], dest_mac[4], dest_mac[5],
                transport == TRANSPORT_AVTP ? "AAF_INT24(BE)" : format_s,
                channels, rate_hz, pps, nominal_spm,
                transport == TRANSPORT_AVTP ? 0 : max_frag_pc,
                nominal_K, max_frame);
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
        const char *label = (transport == TRANSPORT_RTP) ? "rtp" : "ip";
        fprintf(stderr,
                "talker: transport=%s dest=%s:%d family=%s %s\n"
                "        iface=%s ifindex=%d\n"
                "        fmt=%s ch=%d rate=%d pps=%.0f nominal_spp=%.2f max_frag_pc=%d frags/packet=%d max_payload=%zuB feedback=%s%s%s%s\n",
                label, ip_str, udp_port,
                dest_family == AF_INET ? "v4" : "v6",
                dest_is_multicast ? "multicast" : "unicast",
                iface, ifindex,
                transport == TRANSPORT_RTP ? "L24(BE)" : format_s,
                channels, rate_hz,
                pps, nominal_spm, max_frag_pc, nominal_K,
                max_frame - sizeof(struct ether_header),
                feedback_enabled_for_transport ? "on" : "off",
                transport == TRANSPORT_RTP ? " (AES67 relies on PTPv2)" : "",
                transport == TRANSPORT_RTP && sap_fd >= 0  ? " sap=on"  : "",
                transport == TRANSPORT_RTP && ptp_enabled  ? " ptp=TAI" : "");
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
        /* 0. Re-emit SAP announcement every SAP_ANNOUNCE_INTERVAL_S
         *    seconds. If --ptp is on, re-read the PTP grandmaster each
         *    refresh — BMCA can re-elect a different master at any time,
         *    and we need to reflect that in the SDP so controllers
         *    re-evaluate. Changed gmid → bump session_version + rebuild
         *    the SAP packet; identical gmid → just re-send what we have. */
        if (sap_fd >= 0 && sap_pkt_len > 0) {
            struct timespec mono_now;
            clock_gettime(CLOCK_MONOTONIC, &mono_now);
            if (mono_now.tv_sec - last_sap_ts.tv_sec >=
                (time_t)SAP_ANNOUNCE_INTERVAL_S) {

                if (ptp_enabled) {
                    uint8_t gmid_bytes[8];
                    char new_gmid[32] = "";
                    enum sdp_refclk new_refclk =
                        (ptp_pmc_read_gmid(gmid_bytes) == 0)
                            ? SDP_REFCLK_PTP_GMID
                            : SDP_REFCLK_PTP_TRACEABLE;
                    if (new_refclk == SDP_REFCLK_PTP_GMID) {
                        ptp_gmid_to_str(gmid_bytes, new_gmid,
                                        sizeof new_gmid);
                    }
                    if (new_refclk != sdp.refclk ||
                        strcmp(new_gmid, sdp.gmid_str) != 0) {
                        sdp.refclk = new_refclk;
                        strncpy(sdp.gmid_str, new_gmid,
                                sizeof sdp.gmid_str - 1);
                        sdp.gmid_str[sizeof sdp.gmid_str - 1] = '\0';
                        sdp.session_version++;
                        sdp_len = sdp_build(sdp_text, sizeof sdp_text, &sdp);
                        if (sdp_len > 0) {
                            sap_pkt_len = sap_build(
                                sap_pkt, sizeof sap_pkt,
                                SAP_ANNOUNCE,
                                origin_ipv4_be, sap_msg_id,
                                sdp_text, (size_t)sdp_len);
                        }
                    }
                }

                ssize_t n = sendto(sap_fd, sap_pkt, (size_t)sap_pkt_len, 0,
                                   (struct sockaddr *)&sap_to, sizeof sap_to);
                if (n < 0 && errno != EINTR) perror("sendto(SAP refresh)");
                last_sap_ts = mono_now;
            }
        }

        /* 1. Drain any pending feedback frames (non-blocking). RTP/AES67
         * doesn't use AOEther's UAC2-shape feedback (AES67 devices rely
         * on PTPv2 for clocking), so we skip the drain and the
         * rate-recompute for that transport. */
        if (transport == TRANSPORT_RTP) goto skip_feedback;
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
            double new_spm = spms * ((double)packet_period_ns / 1e6);

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
            samples_per_microframe = spms * ((double)packet_period_ns / 1e6);
        } else {
            samples_per_microframe = nominal_spm;
        }

skip_feedback:
        (void)0;   /* null statement so a C compiler accepts the label. */
        /* 3. Wait for timer tick(s). */
        uint64_t ticks;
        ssize_t r = read(tfd, &ticks, sizeof(ticks));
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("timerfd read");
            break;
        }
        if (ticks > 1) late_wakeups++;

        /* 4. Emit `ticks` microframes. Each microframe carries `pc`
         *    bytes-or-samples per channel. In the common case a microframe
         *    becomes one packet; DSD512+ and high-rate multichannel PCM are
         *    split into K back-to-back fragments with consecutive sequence
         *    numbers and last-in-group set only on fragment K-1. AVTP paths
         *    never fragment (single packet per microframe). */
        for (uint64_t i = 0; i < ticks && !g_stop; i++) {
            sample_accum += samples_per_microframe;
            int pc = (int)sample_accum;
            if (pc < 1) pc = 1;
            if (pc > max_samples_per_microframe) pc = max_samples_per_microframe;
            sample_accum -= pc;

            /* Read one microframe of audio into the standalone audio_buf.
             * The source writes `pc` samples/bytes per channel, interleaved
             * per AOE wire format (sample-interleaved for PCM, byte-
             * interleaved for DSD). Slices of this buffer become valid
             * per-fragment payloads with a simple byte-range split, because
             * the first N units per channel are always the first
             * N × channels × bytes_per_sample bytes of the buffer. */
            if (src->read(src, audio_buf, (size_t)pc) < 0) {
                g_stop = 1;
                break;
            }

            /* Fragment count and per-fragment pc distribution. AVTP / RTP
             * are always K=1; AOE uses max_frag_pc computed at startup. */
            int K;
            int base, rem;
            if (transport == TRANSPORT_AVTP || transport == TRANSPORT_RTP) {
                K = 1;
                base = pc;
                rem  = 0;
            } else {
                K = (pc + max_frag_pc - 1) / max_frag_pc;
                if (K < 1) K = 1;
                base = pc / K;
                rem  = pc % K;   /* first `rem` fragments get base+1 */
            }

            /* If AVDECC bound us this tick, steer the destination for all
             * fragments of this microframe together. AVDECC is an L2 / AVTP
             * thing; IP and RTP modes keep their static --dest-ip. */
            int steer_avdecc = 0;
            uint8_t avdecc_mac_snap[6];
            if (transport != TRANSPORT_IP && transport != TRANSPORT_RTP) {
                pthread_mutex_lock(&g_avdecc_mu);
                if (g_avdecc_dest_valid) {
                    memcpy(avdecc_mac_snap, g_avdecc_dest_mac, 6);
                    steer_avdecc = 1;
                }
                pthread_mutex_unlock(&g_avdecc_mu);
                if (steer_avdecc) {
                    struct ether_header *eth = (struct ether_header *)frame;
                    memcpy(eth->ether_dhost, avdecc_mac_snap, 6);
                    memcpy(data_to_ll.sll_addr, avdecc_mac_snap, 6);
                }
            }

            int off_units = 0;
            int abort_microframe = 0;
            for (int k = 0; k < K && !g_stop; k++) {
                const int frag_pc = base + (k < rem ? 1 : 0);
                const size_t frag_payload_bytes =
                    (size_t)frag_pc * channels * bytes_per_sample;
                const size_t audio_offset =
                    (size_t)off_units * channels * bytes_per_sample;

                /* Copy this fragment's audio slice into the frame payload
                 * area. For AVTP K=1 so this copies the whole microframe. */
                memcpy(payload, audio_buf + audio_offset, frag_payload_bytes);

                if (transport == TRANSPORT_AVTP) {
                    /* AAF samples are big-endian on the wire; ALSA is LE.
                     * Swap in place after the memcpy above. */
                    avtp_swap24_inplace(payload,
                                        (size_t)frag_pc * (size_t)channels);
                    avtp_aaf_hdr_build(avtp_hdr_p,
                                       avtp_stream_id,
                                       avtp_seq8++,
                                       0,                    /* avtp_timestamp (no PTP yet) */
                                       AAF_FORMAT_INT24,
                                       avtp_nsr_code,
                                       (uint16_t)channels,
                                       24,                   /* bit_depth */
                                       (uint16_t)frag_payload_bytes);
                } else if (transport == TRANSPORT_RTP) {
                    /* AES67 L24 samples are big-endian on the wire; ALSA
                     * is LE. Swap in place, same as AVTP. */
                    rtp_swap24_inplace(payload,
                                       (size_t)frag_pc * (size_t)channels);
                    rtp_hdr_build(rtp_hdr_p,
                                  RTP_DEFAULT_PT_L24,
                                  rtp_seq16++,
                                  rtp_timestamp,
                                  rtp_ssrc);
                    /* Advance the media clock by one packet's samples for
                     * the next emission. */
                    rtp_timestamp += (uint32_t)frag_pc;
                } else {
                    const uint8_t flags =
                        (k == K - 1) ? AOE_FLAG_LAST_IN_GROUP : 0;
                    aoe_hdr_build(aoe_hdr_p, STREAM_ID, seq, 0,
                                  (uint8_t)channels, format_code,
                                  (uint8_t)frag_pc, flags);
                }

                const size_t frame_len_total =
                    sizeof(struct ether_header) + proto_hdr_len + frag_payload_bytes;

                ssize_t sent;
                if (transport == TRANSPORT_IP ||
                    transport == TRANSPORT_RTP) {
                    sent = sendto(data_sock, frame + tx_offset,
                                  frame_len_total - tx_offset, 0,
                                  (struct sockaddr *)&dest_ss, dest_ss_len);
                } else {
                    sent = sendto(data_sock, frame, frame_len_total, 0,
                                  (struct sockaddr *)&data_to_ll,
                                  sizeof(data_to_ll));
                }
                if (sent < 0) {
                    if (errno == EINTR) { abort_microframe = 1; break; }
                    perror("sendto");
                    g_stop = 1;
                    break;
                }
                seq++;
                off_units += frag_pc;
            }
            if (abort_microframe) break;
        }
    }

    fprintf(stderr,
            "talker: shutting down; sent=%u late_wakeups=%llu fb_rx=%llu fb_ignored=%llu\n",
            seq, (unsigned long long)late_wakeups,
            (unsigned long long)fb_rx, (unsigned long long)fb_ignored);

    /* SAP: one deletion packet so controllers drop the session promptly
     * rather than waiting out the session timeout (~minutes). Best-effort. */
    if (sap_fd >= 0 && sdp_len > 0) {
        uint8_t del_pkt[SDP_MAX_LEN + SAP_HDR_LEN + SAP_MIME_LEN];
        int del_len = sap_build(del_pkt, sizeof del_pkt,
                                SAP_DELETION,
                                origin_ipv4_be, sap_msg_id,
                                sdp_text, (size_t)sdp_len);
        if (del_len > 0) {
            ssize_t n = sendto(sap_fd, del_pkt, (size_t)del_len, 0,
                               (struct sockaddr *)&sap_to, sizeof sap_to);
            (void)n;
        }
        close(sap_fd);
    }

    src->close(src);
    close(tfd);
    if (fb_sock != data_sock) close(fb_sock);
    close(data_sock);
    free(frame);
    free(audio_buf);
    aoether_avdecc_close(avdecc);
    return 0;
}
