#include "avdecc.h"
#include "avtp.h"
#include "mdns.h"
#include "packet.h"
#include "rtp.h"
#include "sap.h"
#include "sdp.h"

#include <pthread.h>

#include <alsa/asoundlib.h>
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Stream format. Channels, rate, and sample format are all runtime-
 * configured from M6 on. See docs/design.md §"M6" for the DSD path.
 * "rate" generalizes to samples-or-DSD-bytes per sec per channel so the
 * per-microframe math matches talker semantics. */
#define STREAM_ID         0x0001

#define DSD64_BYTE_RATE   352800
#define DSD128_BYTE_RATE  705600
#define DSD256_BYTE_RATE  1411200
#define DSD512_BYTE_RATE  2822400     /* M8: carried via per-microframe fragmentation. */
#define DSD1024_BYTE_RATE 5644800
#define DSD2048_BYTE_RATE 11289600

/* Defaults match M1's previous hardcoded values. */
#define DEFAULT_CHANNELS      2
#define DEFAULT_RATE_HZ       48000
#define DEFAULT_LATENCY_US    5000
#define FEEDBACK_PERIOD_MS    20
#define POLL_TIMEOUT_MS       FEEDBACK_PERIOD_MS

/* Packet RX buffer: AOE fragmentation keeps every wire packet ≤ MTU
 * (fragment payload ≤ 1470 B after header overhead), and AVTP keeps its
 * single-packet-per-microframe constraint within the same MTU. 4 KiB
 * comfortably holds one frame plus headers. */
#define RX_BUF_BYTES     4096

/* IP/UDP default port (interim; see docs/wire-format.md). */
#define DEFAULT_UDP_PORT  8805

enum transport_mode {
    TRANSPORT_L2 = 0,
    TRANSPORT_IP = 1,
    TRANSPORT_AVTP = 2,
    TRANSPORT_RTP = 3,   /* RTP/AES67 (Mode 4, M9 Phase A) */
};

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

struct stream_format {
    uint8_t              wire_code;
    int                  bytes_per_sample;
    int                  rate_override;   /* >0 overrides --rate (DSD) */
    int                  is_dsd;
    const char          *name;
};

static int parse_format(const char *s, struct stream_format *f)
{
    if (!s) return -1;
    static const struct stream_format table[] = {
        { AOE_FMT_PCM_S24LE_3,   3, 0,                0, "pcm"    },
        { AOE_FMT_NATIVE_DSD64,  1, DSD64_BYTE_RATE,  1, "dsd64"  },
        { AOE_FMT_NATIVE_DSD128, 1, DSD128_BYTE_RATE, 1, "dsd128" },
        { AOE_FMT_NATIVE_DSD256, 1, DSD256_BYTE_RATE, 1, "dsd256" },
        { AOE_FMT_NATIVE_DSD512, 1, DSD512_BYTE_RATE, 1, "dsd512" },
        { AOE_FMT_NATIVE_DSD1024,1, DSD1024_BYTE_RATE,1, "dsd1024" },
        { AOE_FMT_NATIVE_DSD2048,1, DSD2048_BYTE_RATE,1, "dsd2048" },
    };
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
        if (strcmp(s, table[i].name) == 0) {
            *f = table[i];
            return 0;
        }
    }
    return -1;
}

/* ALSA DSD format selection. The AOE wire format is always per-byte channel-
 * interleaved MSB-first within each byte; that matches DSD_U8 exactly. For
 * the wider DSD_U16_* / DSD_U32_* formats the receiver deinterleaves wire
 * bytes into per-channel linear streams (buffering up to N-1 bytes per
 * channel across packet boundaries) and repacks into ALSA's N-byte-per-
 * channel frames, byte-reversing within each group for the LE variants.
 *
 * PCM uses SND_PCM_FORMAT_S24_3LE with n_bytes=3 and no group reversal —
 * the wire format already delivers those bytes in ALSA's expected order. */
struct alsa_variant {
    const char       *name;
    snd_pcm_format_t  alsa_format;
    int               n_bytes;   /* bytes per channel per ALSA frame */
    int               reverse;   /* 1 → byte-reverse within each N-byte group */
    int               is_dsd;
};

static int parse_alsa_variant(const char *s, struct alsa_variant *v)
{
    if (!s) return -1;
    static const struct alsa_variant table[] = {
        { "pcm_s24_3le", SND_PCM_FORMAT_S24_3LE,    3, 0, 0 },
        { "dsd_u8",      SND_PCM_FORMAT_DSD_U8,     1, 0, 1 },
        { "dsd_u16_be",  SND_PCM_FORMAT_DSD_U16_BE, 2, 0, 1 },
        { "dsd_u16_le",  SND_PCM_FORMAT_DSD_U16_LE, 2, 1, 1 },
        { "dsd_u32_be",  SND_PCM_FORMAT_DSD_U32_BE, 4, 0, 1 },
        { "dsd_u32_le",  SND_PCM_FORMAT_DSD_U32_LE, 4, 1, 1 },
    };
    for (size_t i = 0; i < sizeof(table)/sizeof(table[0]); i++) {
        if (strcmp(s, table[i].name) == 0) {
            *v = table[i];
            return 0;
        }
    }
    return -1;
}

static volatile sig_atomic_t g_stop;

/* AVDECC bind state. An AVDECC controller (Hive) issuing CONNECT_RX on
 * this listener fires on_bind from la_avdecc's executor thread; the
 * main data-path loop reads this under the mutex and prefers the
 * announced talker MAC over the first-frame-learned one. */
static pthread_mutex_t g_avdecc_mu  = PTHREAD_MUTEX_INITIALIZER;
static uint8_t         g_avdecc_peer_mac[6];
static int             g_avdecc_peer_valid;

static void avdecc_on_bind(const uint8_t mac[6], uint64_t stream_id, void *user)
{
    (void)stream_id; (void)user;
    pthread_mutex_lock(&g_avdecc_mu);
    memcpy(g_avdecc_peer_mac, mac, 6);
    g_avdecc_peer_valid = 1;
    pthread_mutex_unlock(&g_avdecc_mu);
}

static void avdecc_on_unbind(void *user)
{
    (void)user;
    pthread_mutex_lock(&g_avdecc_mu);
    g_avdecc_peer_valid = 0;
    pthread_mutex_unlock(&g_avdecc_mu);
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s --iface IF --dac hw:CARD=NAME,DEV=0 [options]\n"
        "  --transport l2|ip|avtp|rtp   transport mode, default l2\n"
        "                       (avtp = IEEE 1722 AAF, Milan interop;\n"
        "                        rtp  = RTP/AES67 per M9 Phase A)\n"
        "  --port N             UDP port (ip mode default %d; rtp default 5004)\n"
        "  --group IP           multicast group to join (ip/rtp modes, optional;\n"
        "                       IPv4 in 224.0.0.0/4 or IPv6 in ff00::/8)\n"
        "  --format FMT         pcm | dsd64 | dsd128 | dsd256\n"
        "                       | dsd512 | dsd1024 | dsd2048\n"
        "                       default pcm. AVTP transport is pcm-only.\n"
        "                       DSD512+ arrives as split fragments — see\n"
        "                       docs/wire-format.md §\"Cadence\".\n"
        "  --alsa-format FMT    ALSA sample format. Default picks from --format:\n"
        "                         pcm → pcm_s24_3le; dsd* → dsd_u8.\n"
        "                       DSD override: dsd_u8 | dsd_u16_le | dsd_u16_be |\n"
        "                                     dsd_u32_le | dsd_u32_be.\n"
        "                       Match whatever your DAC's snd_usb_audio quirk exposes;\n"
        "                       the receiver handles the transpose + endian conversion.\n"
        "  --channels N         stream channel count (1..64, default %d)\n"
        "  --rate HZ            PCM only: 44100|48000|88200|96000|176400|192000\n"
        "                       (default %d; ignored for DSD — rate is implied by --format)\n"
        "  --latency-us N       ALSA period latency hint (default %d)\n"
        "  --no-feedback        do not emit Mode C FEEDBACK frames (diagnostic)\n"
        "  --announce           publish this receiver via mDNS-SD (_aoether._udp)\n"
        "                       so talkers and avahi-browse can discover it\n"
        "  --name NAME          instance name for --announce and --avdecc (default: hostname)\n"
        "  --avdecc             start an AVDECC listener entity (Milan/Hive discovery);\n"
        "                       requires la_avdecc submodule built — see docs/recipe-avdecc.md\n"
        "  --list-sap [SECS]    passive AES67 SAP listener: join 239.255.255.255:9875,\n"
        "                       dedupe sessions by (origin, msg_id), print each one\n"
        "                       with a ready-to-run receiver command, then exit.\n"
        "                       SECS (default 5) is the listen window. Does not open\n"
        "                       the DAC; --iface is the only other required option.\n"
        "  --sdp PATH           M10 Phase C: read an AES67 SDP from PATH. Single-m=\n"
        "                       SDPs are applied as --transport rtp + --group / --port /\n"
        "                       --channels / --rate overrides. Bundled (multi-m=) SDPs\n"
        "                       are parsed and their substream layout is logged; full\n"
        "                       multi-stream receive lands with Phase D hardware interop.\n",
        prog, DEFAULT_UDP_PORT, DEFAULT_CHANNELS, DEFAULT_RATE_HZ, DEFAULT_LATENCY_US);
}

static int64_t ts_diff_ns(struct timespec a, struct timespec b)
{
    return (int64_t)(a.tv_sec - b.tv_sec) * 1000000000LL +
           (int64_t)(a.tv_nsec - b.tv_nsec);
}

static int iface_lookup(int sock, const char *name, int *ifindex, uint8_t mac[6])
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
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
}

int main(int argc, char **argv)
{
    const char *iface = NULL;
    const char *dac = NULL;
    const char *group_s = NULL;
    const char *format_s = "pcm";
    const char *alsa_format_s = NULL;   /* resolved below from --format if NULL */
    int channels = DEFAULT_CHANNELS;
    int rate_hz = DEFAULT_RATE_HZ;
    int latency_us = DEFAULT_LATENCY_US;
    int feedback_enabled = 1;
    enum transport_mode transport = TRANSPORT_L2;
    int udp_port = DEFAULT_UDP_PORT;
    int announce = 0;
    const char *announce_name = NULL;
    int avdecc_enabled = 0;
    int list_sap = 0;
    int list_sap_secs = 5;
    const char *sdp_path = NULL;

    static const struct option opts[] = {
        { "iface",       required_argument, 0, 'i' },
        { "dac",         required_argument, 0, 'd' },
        { "transport",   required_argument, 0, 'T' },
        { "port",        required_argument, 0, 'P' },
        { "group",       required_argument, 0, 'G' },
        { "channels",    required_argument, 0, 'C' },
        { "rate",        required_argument, 0, 'r' },
        { "format",      required_argument, 0, 'F' },
        { "alsa-format", required_argument, 0, 'A' },
        { "latency-us",  required_argument, 0, 'l' },
        { "no-feedback", no_argument,       0, 'n' },
        { "announce",    no_argument,       0, 1001 },
        { "name",        required_argument, 0, 'N' },
        { "avdecc",      no_argument,       0, 'V' },
        { "list-sap",    optional_argument, 0, 1002 },
        { "sdp",         required_argument, 0, 1003 },
        { "help",        no_argument,       0, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:d:T:P:G:C:r:F:l:nAN:Vh", opts, NULL)) != -1) {
        switch (c) {
        case 'i': iface = optarg; break;
        case 'd': dac = optarg; break;
        case 'T':
            if      (strcmp(optarg, "l2") == 0)   transport = TRANSPORT_L2;
            else if (strcmp(optarg, "ip") == 0)   transport = TRANSPORT_IP;
            else if (strcmp(optarg, "avtp") == 0) transport = TRANSPORT_AVTP;
            else if (strcmp(optarg, "rtp") == 0)  transport = TRANSPORT_RTP;
            else { fprintf(stderr, "receiver: --transport must be l2, ip, avtp, or rtp\n"); return 2; }
            break;
        case 'P': udp_port = atoi(optarg); break;
        case 'G': group_s = optarg; break;
        case 'C': channels = atoi(optarg); break;
        case 'r': rate_hz = atoi(optarg); break;
        case 'F': format_s = optarg; break;
        case 'A': alsa_format_s = optarg; break;
        case 'l': latency_us = atoi(optarg); break;
        case 'n': feedback_enabled = 0; break;
        case 1001: announce = 1; break;
        case 'N': announce_name = optarg; break;
        case 'V': avdecc_enabled = 1; break;
        case 1002:
            list_sap = 1;
            if (optarg) list_sap_secs = atoi(optarg);
            if (list_sap_secs < 1) list_sap_secs = 1;
            break;
        case 1003: sdp_path = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (!iface) {
        usage(argv[0]);
        return 2;
    }
    if (!list_sap && !dac) {
        usage(argv[0]);
        return 2;
    }

    /* M10 Phase C — ingest a bundled SDP. Single-m= SDPs are used as a
     * convenience: parsed fields override --group / --port / --channels
     * / --rate so the operator can drop a controller-issued SDP into
     * the receiver without hand-copying numbers. Multi-m= SDPs are
     * parsed and the substream layout is printed, but the receive loop
     * itself is deferred — landing full reassembly + multi-socket poll
     * alongside hardware interop (Phase D) keeps untested code out of
     * the tree. */
    if (sdp_path) {
        FILE *fp = fopen(sdp_path, "rb");
        if (!fp) {
            fprintf(stderr, "receiver: --sdp %s: %s\n",
                    sdp_path, strerror(errno));
            return 2;
        }
        char sdp_buf[SDP_MAX_LEN];
        size_t sdp_n = fread(sdp_buf, 1, sizeof sdp_buf - 1, fp);
        fclose(fp);
        if (sdp_n == 0) {
            fprintf(stderr, "receiver: --sdp %s is empty\n", sdp_path);
            return 2;
        }
        sdp_buf[sdp_n] = '\0';

        struct sdp_params  session;
        struct sdp_media_parsed media[16];
        size_t n_media = 0;
        if (sdp_parse_bundle(sdp_buf, sdp_n,
                             &session, media,
                             sizeof media / sizeof media[0],
                             &n_media) < 0) {
            fprintf(stderr,
                    "receiver: --sdp %s: could not parse as AES67 SDP\n",
                    sdp_path);
            return 2;
        }

        fprintf(stderr,
                "receiver: --sdp parsed: session \"%s\", %zu substream%s, "
                "%s refclk\n",
                session.session_name[0] ? session.session_name : "(unnamed)",
                n_media, n_media == 1 ? "" : "s",
                session.refclk == SDP_REFCLK_PTP_GMID     ? "ptp:gmid" :
                session.refclk == SDP_REFCLK_PTP_TRACEABLE ? "ptp:traceable" :
                                                             "none");
        int total_channels = 0;
        for (size_t i = 0; i < n_media; i++) {
            fprintf(stderr,
                    "receiver:   substream %zu (mid=%d): %u ch @ %u Hz "
                    "L%s → %s:%u ptime=%u.%03u ms\n",
                    i, media[i].mid,
                    (unsigned)media[i].channels,
                    (unsigned)media[i].sample_rate_hz,
                    media[i].encoding == SDP_ENC_L16 ? "16" : "24",
                    media[i].dest_ip[0] ? media[i].dest_ip : "(no c=)",
                    (unsigned)media[i].port,
                    (unsigned)(media[i].ptime_us / 1000),
                    (unsigned)(media[i].ptime_us % 1000));
            total_channels += media[i].channels;
        }

        if (n_media > 1) {
            fprintf(stderr,
                    "receiver: bundled SDP with %zu substreams (total %d "
                    "channels) parsed successfully. Multi-stream receive "
                    "(M10 Phase C receive-side reassembly) is not yet "
                    "wired through the main data-path; it lands alongside "
                    "hardware interop validation (Phase D). For today's "
                    "test rigs: run one receiver per substream with "
                    "--group pointing at that substream's multicast "
                    "address.\n",
                    n_media, total_channels);
            return 2;
        }

        /* Single-m= SDP: inject its fields as if --group / --port /
         * --channels / --rate had been provided. Explicit CLI values
         * win if the user also set them. */
        transport = TRANSPORT_RTP;
        if (!group_s && media[0].dest_ip[0]) group_s = media[0].dest_ip;
        if (udp_port == DEFAULT_UDP_PORT && media[0].port)
            udp_port = media[0].port;
        if (channels == DEFAULT_CHANNELS && media[0].channels)
            channels = media[0].channels;
        if (rate_hz == DEFAULT_RATE_HZ && media[0].sample_rate_hz)
            rate_hz = media[0].sample_rate_hz;
        fprintf(stderr,
                "receiver: --sdp single-stream: applied "
                "--transport rtp --group %s --port %u --channels %d "
                "--rate %d\n",
                group_s ? group_s : "(unset)", udp_port, channels, rate_hz);
    }

    /* --list-sap: sniff SAP announcements (both IPv4 and IPv6) and print
     * each unique session with a ready-to-run receiver command, then exit.
     * Does not touch ALSA or any transport sockets. Each SAP family runs
     * on its own socket: IPv4 joins 239.255.255.255 and IPv6 joins
     * ff0e::2:7ffe. We accept whichever (or both) the kernel can open. */
    if (list_sap) {
        int sfd_v4 = sap_open_rx_socket(AF_INET, iface);
        int sfd_v6 = sap_open_rx_socket(AF_INET6, iface);
        if (sfd_v4 < 0 && sfd_v6 < 0) {
            fprintf(stderr,
                    "receiver: could not open any SAP RX socket (v4 or v6) "
                    "on %s\n", iface);
            return 1;
        }

        fprintf(stderr,
                "receiver: listening for SAP announcements on %s for %d s\n"
                "          v4: %s%s\n"
                "          v6: %s%s\n",
                iface, list_sap_secs,
                sfd_v4 >= 0 ? SAP_IPV4_ADDR_STR : "(unavailable)",
                sfd_v4 >= 0 ? ":9875" : "",
                sfd_v6 >= 0 ? "[" SAP_IPV6_ADDR_STR "]" : "(unavailable)",
                sfd_v6 >= 0 ? ":9875" : "");

        struct seen {
            int family;
            union { uint32_t v4_be; uint8_t v6[16]; } origin;
            uint16_t msg_id;
        };
        enum { SEEN_CAP = 64 };
        struct seen seen_tbl[SEEN_CAP];
        int seen_n = 0;
        int printed = 0;

        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (;;) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long remaining_ms =
                list_sap_secs * 1000
                - (long)((now.tv_sec - t0.tv_sec) * 1000
                         + (now.tv_nsec - t0.tv_nsec) / 1000000);
            if (remaining_ms <= 0) break;

            struct pollfd pfds[2];
            int npfds = 0;
            int idx_v4 = -1, idx_v6 = -1;
            if (sfd_v4 >= 0) {
                pfds[npfds].fd = sfd_v4;
                pfds[npfds].events = POLLIN;
                idx_v4 = npfds++;
            }
            if (sfd_v6 >= 0) {
                pfds[npfds].fd = sfd_v6;
                pfds[npfds].events = POLLIN;
                idx_v6 = npfds++;
            }

            int pr = poll(pfds, npfds,
                          remaining_ms < 1000 ? (int)remaining_ms : 1000);
            if (pr < 0) {
                if (errno == EINTR) continue;
                perror("poll(sap)");
                break;
            }
            if (pr == 0) continue;

            for (int p = 0; p < npfds; p++) {
                if (!(pfds[p].revents & POLLIN)) continue;
                int rfd = pfds[p].fd;

                uint8_t buf[4096];
                struct sockaddr_storage src_addr;
                socklen_t src_len = sizeof src_addr;
                ssize_t n = recvfrom(rfd, buf, sizeof buf, 0,
                                     (struct sockaddr *)&src_addr, &src_len);
                if (n <= 0) continue;
                (void)idx_v4; (void)idx_v6;   /* indices kept for clarity */

                enum sap_kind kind;
                struct sap_origin origin;
                uint16_t msg_id;
                const char *sdp_text;
                size_t sdp_len;
                if (sap_parse(buf, (size_t)n, &kind, &origin, &msg_id,
                              &sdp_text, &sdp_len) < 0) {
                    continue;
                }
                if (kind == SAP_DELETION) continue;

                int dup = 0;
                for (int i = 0; i < seen_n; i++) {
                    if (seen_tbl[i].family != origin.family) continue;
                    if (seen_tbl[i].msg_id != msg_id) continue;
                    int match = (origin.family == AF_INET)
                        ? (seen_tbl[i].origin.v4_be == origin.addr.v4_be)
                        : (memcmp(seen_tbl[i].origin.v6, origin.addr.v6, 16) == 0);
                    if (match) { dup = 1; break; }
                }
                if (dup) continue;
                if (seen_n < SEEN_CAP) {
                    seen_tbl[seen_n].family = origin.family;
                    seen_tbl[seen_n].msg_id = msg_id;
                    if (origin.family == AF_INET)
                        seen_tbl[seen_n].origin.v4_be = origin.addr.v4_be;
                    else
                        memcpy(seen_tbl[seen_n].origin.v6,
                               origin.addr.v6, 16);
                    seen_n++;
                }

                struct sdp_params sp;
                if (sdp_parse(sdp_text, sdp_len, &sp) < 0) continue;

                char origin_str[INET6_ADDRSTRLEN] = "?";
                if (origin.family == AF_INET)
                    inet_ntop(AF_INET, &origin.addr.v4_be,
                              origin_str, sizeof origin_str);
                else
                    inet_ntop(AF_INET6, origin.addr.v6,
                              origin_str, sizeof origin_str);

                printed++;
                printf("\n  [%d] session=\"%s\"\n", printed, sp.session_name);
                printf("      origin=%s  dest=%s:%u  family=%s\n",
                       origin_str,
                       sp.dest_ip ? sp.dest_ip : "?",
                       (unsigned)sp.port,
                       sp.family == SDP_ADDR_IP6 ? "IP6" : "IP4");
                printf("      fmt=%s rate=%u ch=%u pt=%u ptime=%.3fms refclk=%s\n",
                       sp.encoding == SDP_ENC_L16 ? "L16" : "L24",
                       (unsigned)sp.sample_rate_hz,
                       (unsigned)sp.channels,
                       (unsigned)sp.payload_type,
                       (double)sp.ptime_us / 1000.0,
                       sp.refclk == SDP_REFCLK_PTP_TRACEABLE ? "ptp" : "none");
                printf("      cmd:\n");
                printf("        sudo receiver --iface %s --dac hw:CARD=...,DEV=0 \\\n"
                       "                      --transport rtp --group %s --port %u \\\n"
                       "                      --channels %u --rate %u\n",
                       iface,
                       sp.dest_ip ? sp.dest_ip
                           : (sp.family == SDP_ADDR_IP6 ? "ff3e::X" : "239.X.X.X"),
                       (unsigned)sp.port,
                       (unsigned)sp.channels,
                       (unsigned)sp.sample_rate_hz);
            }
        }
        if (sfd_v4 >= 0) close(sfd_v4);
        if (sfd_v6 >= 0) close(sfd_v6);
        if (printed == 0) {
            fprintf(stderr,
                    "receiver: no SAP announcements seen in %d s — check\n"
                    "  * the talker is running with --announce-sap\n"
                    "  * the switch/router doesn't block multicast to\n"
                    "    %s (v4) or %s (v6)\n"
                    "  * the right --iface is selected\n",
                    list_sap_secs,
                    SAP_IPV4_ADDR_STR, SAP_IPV6_ADDR_STR);
            return 1;
        }
        return 0;
    }
    if (udp_port < 1 || udp_port > 65535) {
        fprintf(stderr, "receiver: --port out of range\n");
        return 2;
    }
    if (transport != TRANSPORT_IP && transport != TRANSPORT_RTP && group_s) {
        fprintf(stderr, "receiver: --group only makes sense with --transport ip or rtp\n");
        return 2;
    }
    if (transport == TRANSPORT_RTP && udp_port == DEFAULT_UDP_PORT) {
        /* AES67 registered RTP port is 5004. */
        udp_port = RTP_DEFAULT_PORT;
    }

    if (channels < 1 || channels > 64) {
        fprintf(stderr, "receiver: --channels must be 1..64 (got %d)\n", channels);
        return 2;
    }

    struct stream_format fmt;
    if (parse_format(format_s, &fmt) < 0) {
        fprintf(stderr, "receiver: unknown --format %s\n", format_s);
        return 2;
    }
    if (fmt.is_dsd) {
        rate_hz = fmt.rate_override;
    } else if (!rate_supported(rate_hz)) {
        fprintf(stderr, "receiver: unsupported PCM --rate %d\n", rate_hz);
        return 2;
    }
    if (transport == TRANSPORT_AVTP && fmt.is_dsd) {
        fprintf(stderr, "receiver: AVTP AAF does not carry DSD; use --transport l2 or ip\n");
        return 2;
    }
    if (transport == TRANSPORT_RTP && fmt.is_dsd) {
        fprintf(stderr, "receiver: AES67 RTP is PCM-only; use --transport l2 or ip with --format dsd*\n");
        return 2;
    }

    /* AVTP AAF only carries the standard NSR rates. */
    uint8_t avtp_nsr_code = 0;
    if (transport == TRANSPORT_AVTP) {
        if (avtp_aaf_nsr_from_hz(rate_hz, &avtp_nsr_code) < 0) {
            fprintf(stderr, "receiver: AVTP AAF has no NSR code for rate %d\n", rate_hz);
            return 2;
        }
    }
    const int bytes_per_sample = fmt.bytes_per_sample;
    const uint8_t expected_format_code = fmt.wire_code;

    /* Resolve ALSA variant. Default derives from --format; user can override
     * for DSD when the DAC's snd_usb_audio quirk exposes only DSD_U16 / U32. */
    struct alsa_variant av;
    if (!alsa_format_s) alsa_format_s = fmt.is_dsd ? "dsd_u8" : "pcm_s24_3le";
    if (parse_alsa_variant(alsa_format_s, &av) < 0) {
        fprintf(stderr, "receiver: unknown --alsa-format %s\n", alsa_format_s);
        return 2;
    }
    if (av.is_dsd != fmt.is_dsd) {
        fprintf(stderr,
                "receiver: --alsa-format %s is %s but --format %s is %s\n",
                alsa_format_s, av.is_dsd ? "DSD" : "PCM",
                fmt.name, fmt.is_dsd ? "DSD" : "PCM");
        return 2;
    }

    /* Socket setup per transport. L2 keeps two raw AF_PACKET sockets (one
     * per EtherType). IP uses one SOCK_DGRAM socket bound to udp_port —
     * receives data and sends FEEDBACK; magic byte distinguishes. */
    int data_sock = -1;
    int ctl_sock = -1;    /* == data_sock in IP mode */
    int ifindex = 0;
    uint8_t my_mac[6] = {0};
    int group_family = AF_UNSPEC;
    int use_multicast = 0;

    if (transport == TRANSPORT_L2 || transport == TRANSPORT_AVTP) {
        uint16_t data_etype = (transport == TRANSPORT_AVTP) ? AVTP_ETHERTYPE
                                                            : AOE_ETHERTYPE;
        data_sock = socket(AF_PACKET, SOCK_RAW, htons(data_etype));
        if (data_sock < 0) {
            perror("socket(AF_PACKET, SOCK_RAW) data");
            fprintf(stderr, "receiver: raw sockets need CAP_NET_RAW (try sudo)\n");
            return 1;
        }
        if (iface_lookup(data_sock, iface, &ifindex, my_mac) < 0) return 1;
        struct sockaddr_ll bind_ll = {
            .sll_family = AF_PACKET,
            .sll_protocol = htons(data_etype),
            .sll_ifindex = ifindex,
        };
        if (bind(data_sock, (struct sockaddr *)&bind_ll, sizeof(bind_ll)) < 0) {
            perror("bind(data_sock)");
            return 1;
        }
        /* Mode C feedback is unchanged across L2 / AVTP — both ride 0x88B6. */
        ctl_sock = socket(AF_PACKET, SOCK_RAW, htons(AOE_C_ETHERTYPE));
        if (ctl_sock < 0) {
            perror("socket(AF_PACKET, SOCK_RAW) control");
            return 1;
        }
    } else {
        /* Decide v4 vs v6 by --group (if any) or default to v4. */
        struct in_addr  group_v4 = { 0 };
        struct in6_addr group_v6 = { 0 };
        int family = AF_INET;
        if (group_s) {
            if (inet_pton(AF_INET, group_s, &group_v4) == 1) {
                family = AF_INET;
                uint8_t first = ((uint8_t *)&group_v4)[0];
                use_multicast = (first >= 224 && first <= 239);
                if (!use_multicast) {
                    fprintf(stderr, "receiver: --group %s is not IPv4 multicast (224/4)\n", group_s);
                    return 2;
                }
            } else if (inet_pton(AF_INET6, group_s, &group_v6) == 1) {
                family = AF_INET6;
                use_multicast = (group_v6.s6_addr[0] == 0xff);
                if (!use_multicast) {
                    fprintf(stderr, "receiver: --group %s is not IPv6 multicast (ff00::/8)\n", group_s);
                    return 2;
                }
            } else {
                fprintf(stderr, "receiver: --group %s is not a valid IP literal\n", group_s);
                return 2;
            }
        }
        group_family = family;

        data_sock = socket(family, SOCK_DGRAM, 0);
        if (data_sock < 0) {
            perror("socket(AF_INET*, SOCK_DGRAM)");
            return 1;
        }

        /* SO_REUSEADDR lets multiple receivers bind the same port on a
         * multicast group. */
        int one = 1;
        setsockopt(data_sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

        /* Interface ifindex needed for multicast membership. */
        if (iface_lookup(data_sock, iface, &ifindex, my_mac) < 0) return 1;

        if (family == AF_INET) {
            struct sockaddr_in bind_sin = { .sin_family = AF_INET };
            bind_sin.sin_addr.s_addr = htonl(INADDR_ANY);
            bind_sin.sin_port = htons((uint16_t)udp_port);
            if (bind(data_sock, (struct sockaddr *)&bind_sin, sizeof(bind_sin)) < 0) {
                perror("bind(udp v4)");
                return 1;
            }
            if (use_multicast) {
                struct ip_mreqn mreq;
                memset(&mreq, 0, sizeof(mreq));
                mreq.imr_multiaddr = group_v4;
                mreq.imr_ifindex = ifindex;
                if (setsockopt(data_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                               &mreq, sizeof(mreq)) < 0) {
                    perror("setsockopt(IP_ADD_MEMBERSHIP)");
                    return 1;
                }
            }
        } else {
            struct sockaddr_in6 bind_sin6 = { .sin6_family = AF_INET6 };
            bind_sin6.sin6_addr = in6addr_any;
            bind_sin6.sin6_port = htons((uint16_t)udp_port);
            if (bind(data_sock, (struct sockaddr *)&bind_sin6, sizeof(bind_sin6)) < 0) {
                perror("bind(udp v6)");
                return 1;
            }
            if (use_multicast) {
                struct ipv6_mreq mreq6;
                memset(&mreq6, 0, sizeof(mreq6));
                mreq6.ipv6mr_multiaddr = group_v6;
                mreq6.ipv6mr_interface = (unsigned)ifindex;
                if (setsockopt(data_sock, IPPROTO_IPV6, IPV6_JOIN_GROUP,
                               &mreq6, sizeof(mreq6)) < 0) {
                    perror("setsockopt(IPV6_JOIN_GROUP)");
                    return 1;
                }
            }
        }
        ctl_sock = data_sock;   /* one socket for RX data and TX feedback */
    }

    /* ALSA: open DAC, set the M1-hardcoded format. */
    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, dac, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_open(%s): %s\n", dac, snd_strerror(err));
        return 1;
    }
    /* ALSA's "frame rate" differs from wire rate_hz for wider DSD formats:
     * each ALSA frame consumes av.n_bytes bytes per channel, so
     *   ALSA rate = (wire DSD byte rate per channel) / av.n_bytes.
     * For PCM S24_3LE the two concepts coincide. */
    const unsigned alsa_rate = fmt.is_dsd
        ? (unsigned)rate_hz / (unsigned)av.n_bytes
        : (unsigned)rate_hz;
    err = snd_pcm_set_params(pcm,
                             av.alsa_format,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             (unsigned int)channels,
                             alsa_rate,
                             0,              /* disable ALSA soft-resample */
                             (unsigned int)latency_us);
    if (err < 0) {
        fprintf(stderr,
                "snd_pcm_set_params (ch=%d alsa_rate=%u fmt=%s alsa=%s): %s\n"
                "  (DAC must natively support this configuration; "
                "AOEther never resamples.  For DSD, try a different\n"
                "  --alsa-format matching your DAC's snd_usb_audio quirk\n"
                "  — common variants are dsd_u8, dsd_u16_le, dsd_u32_be.)\n",
                channels, alsa_rate, fmt.name, alsa_format_s, snd_strerror(err));
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    const int feedback_effective =
        feedback_enabled && transport != TRANSPORT_RTP;
    if (transport == TRANSPORT_L2 || transport == TRANSPORT_AVTP) {
        fprintf(stderr,
                "receiver: transport=%s iface=%s dac=%s fmt=%s alsa=%s ch=%d rate=%d alsa_rate=%u latency_us=%d feedback=%s\n",
                transport == TRANSPORT_AVTP ? "avtp" : "l2",
                iface, dac,
                transport == TRANSPORT_AVTP ? "AAF_INT24(BE)" : fmt.name,
                av.name,
                channels, rate_hz, alsa_rate, latency_us,
                feedback_effective ? "on" : "off");
    } else {
        const char *label = (transport == TRANSPORT_RTP) ? "rtp" : "ip";
        const char *wire_fmt =
            (transport == TRANSPORT_RTP) ? "L24(BE)" : fmt.name;
        fprintf(stderr,
                "receiver: transport=%s family=%s %s port=%d%s%s\n"
                "          iface=%s dac=%s fmt=%s alsa=%s ch=%d rate=%d alsa_rate=%u latency_us=%d feedback=%s%s\n",
                label,
                group_family == AF_INET6 ? "v6" : "v4",
                use_multicast ? "multicast" : "unicast",
                udp_port,
                group_s ? " group=" : "",
                group_s ? group_s : "",
                iface, dac, wire_fmt, av.name,
                channels, rate_hz, alsa_rate, latency_us,
                feedback_effective ? "on" : "off",
                transport == TRANSPORT_RTP ? " (AES67: relies on PTPv2)" : "");
    }

    /* mDNS-SD: announce this receiver and its DAC capabilities so talkers
     * (and avahi-browse) can discover it without static configuration.
     * See docs/recipe-discovery.md. Publication is best-effort; if avahi
     * isn't present we log and carry on. */
    struct aoether_mdns *mdns = NULL;
    if (announce) {
        char hostname[128];
        if (!announce_name) {
            if (gethostname(hostname, sizeof(hostname)) == 0) {
                hostname[sizeof(hostname) - 1] = 0;
                announce_name = hostname;
            } else {
                announce_name = "aoether-receiver";
            }
        }
        char ch_s[8], rate_s[16], port_s[8];
        snprintf(ch_s,   sizeof(ch_s),   "%d", channels);
        snprintf(rate_s, sizeof(rate_s), "%d", rate_hz);
        snprintf(port_s, sizeof(port_s), "%d", udp_port);
        const char *transport_s =
            transport == TRANSPORT_L2   ? "l2"   :
            transport == TRANSPORT_AVTP ? "avtp" : "ip";
        struct aoether_mdns_txt txt[] = {
            { "ver",       "1" },
            { "role",      "receiver" },
            { "transport", transport_s },
            { "format",    fmt.name },
            { "channels",  ch_s },
            { "rate",      rate_s },
            { "iface",     iface },
            { "dac",       dac },
            { "port",      port_s },
        };
        mdns = aoether_mdns_publish("_aoether._udp",
                                    announce_name,
                                    (uint16_t)udp_port,
                                    txt,
                                    sizeof(txt) / sizeof(txt[0]));
        if (!mdns) {
            fprintf(stderr,
                    "receiver: mDNS-SD publication failed (continuing without announce)\n");
        }
    }

    /* AVDECC entity (M7 Phase B). Milan controllers like Hive discover
     * AOEther listeners through this entity and use ACMP CONNECT_RX to
     * bind a peer talker at runtime. Currently scaffolding only — the
     * descriptor tree and ACMP handler are filled in by step 2. */
    if (avdecc_enabled && transport == TRANSPORT_RTP) {
        fprintf(stderr,
                "receiver: --avdecc has no effect under --transport rtp "
                "(AES67 uses SAP/SDP for discovery; AVDECC is Milan-only)\n");
        avdecc_enabled = 0;
    }
    struct aoether_avdecc *avdecc = NULL;
    if (avdecc_enabled) {
        struct aoether_avdecc_config cfg = {
            .role        = AOETHER_AVDECC_LISTENER,
            .entity_name = announce_name,
            .iface       = iface,
            .channels    = channels,
            .rate_hz     = rate_hz,
            .format_name = fmt.name,
        };
        avdecc = aoether_avdecc_open(&cfg, avdecc_on_bind, avdecc_on_unbind, NULL);
        if (!avdecc) {
            fprintf(stderr,
                    "receiver: AVDECC entity open failed (continuing without --avdecc)\n");
        }
    }

    /* Data-path buffer and counters. */
    uint8_t buf[RX_BUF_BYTES];
    uint32_t last_seq = 0;
    int have_seq = 0;
    uint64_t rx = 0, dropped = 0, lost = 0, underruns = 0;

    /* DSD repack scratch for the DSD_U16 / DSD_U32 paths. Worst case is
     * channels=64 × (n_bytes-1=3 leftover + max payload_count=255) = 16512
     * bytes per channel view; round up. For DSD_U8 / PCM these are unused. */
    uint8_t dsd_per_ch[64 * (3 + 256)];
    uint8_t dsd_out[RX_BUF_BYTES];
    uint8_t dsd_leftover[64 * 3];
    int     dsd_leftover_per_ch = 0;

    /* Mode C state. */
    uint64_t frames_written_total = 0;
    uint64_t last_consumed = 0;
    struct timespec last_fb_ts = { 0, 0 };
    int rate_bootstrapped = 0;

    /* Talker address as learned from the first valid data frame. For L2
     * this is a MAC; for IP it's a sockaddr_storage + port. */
    uint8_t talker_mac[6];
    struct sockaddr_storage talker_addr;
    socklen_t talker_addr_len = 0;
    int have_talker = 0;

    uint16_t fb_seq = 0;
    uint64_t fb_sent = 0;

    /* Feedback frame template: for L2 it's Ethernet+AoE-C (30B, kernel pads
     * to 60); for IP it's just AoE-C (16B), kernel adds UDP/IP. */
    uint8_t fb_frame[60];
    memset(fb_frame, 0, sizeof(fb_frame));
    struct ether_header *fb_eth = (struct ether_header *)fb_frame;
    struct aoe_c_hdr *fb_hdr_l2 =
        (struct aoe_c_hdr *)(fb_frame + sizeof(struct ether_header));
    struct aoe_c_hdr *fb_hdr_ip =
        (struct aoe_c_hdr *)fb_frame;
    if (transport != TRANSPORT_IP) {
        memcpy(fb_eth->ether_shost, my_mac, 6);
        fb_eth->ether_type = htons(AOE_C_ETHERTYPE);
    }

    struct sockaddr_ll fb_to_ll = {
        .sll_family = AF_PACKET,
        .sll_protocol = htons(AOE_C_ETHERTYPE),
        .sll_ifindex = ifindex,
        .sll_halen = 6,
    };

    struct pollfd pfd = { .fd = data_sock, .events = POLLIN };

    while (!g_stop) {
        int pr = poll(&pfd, 1, POLL_TIMEOUT_MS);
        if (pr < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        if (pfd.revents & POLLIN) {
            struct sockaddr_storage src_addr;
            socklen_t src_addrlen = sizeof(src_addr);
            ssize_t n;
            if (transport == TRANSPORT_IP || transport == TRANSPORT_RTP) {
                n = recvfrom(data_sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src_addr, &src_addrlen);
            } else {
                n = recv(data_sock, buf, sizeof(buf), 0);
            }
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("recv");
                break;
            }

            /* Parse the protocol header into a uniform (frames, payload_ptr,
             * sequence) tuple, then fall through to the common ALSA-write
             * and talker-learning path. */
            size_t hdr_off = (transport == TRANSPORT_IP || transport == TRANSPORT_RTP)
                                 ? 0 : sizeof(struct ether_header);
            size_t frames = 0;
            size_t payload_bytes = 0;
            uint8_t *payload_p = NULL;
            uint32_t seq = 0;

            if (transport == TRANSPORT_RTP) {
                /* RTP/AES67 path: 12-byte RTP header then L24 big-endian
                 * PCM. No AoE header, no magic byte — the receiver decides
                 * this is RTP from --transport rtp. Format / channel
                 * validation comes from SDP in a future milestone;
                 * for M9 Phase A we trust --rate / --channels / L24. */
                if ((size_t)n < hdr_off + RTP_HDR_LEN) { dropped++; goto check_feedback; }
                const struct rtp_hdr *rh =
                    (const struct rtp_hdr *)(buf + hdr_off);
                uint8_t  rpt;
                uint16_t rseq;
                uint32_t rts, rssrc;
                if (rtp_hdr_parse(rh, &rpt, &rseq, &rts, &rssrc) < 0) {
                    dropped++; goto check_feedback;
                }
                (void)rts; (void)rssrc;
                payload_bytes = (size_t)n - hdr_off - RTP_HDR_LEN;
                size_t per_sample = (size_t)channels * bytes_per_sample;
                if (per_sample == 0 || payload_bytes == 0 ||
                    payload_bytes % per_sample != 0) {
                    dropped++; goto check_feedback;
                }
                frames = payload_bytes / per_sample;
                payload_p = buf + hdr_off + RTP_HDR_LEN;
                /* L24 samples are big-endian; swap to ALSA LE in place. */
                rtp_swap24_inplace(payload_p, frames * (size_t)channels);
                /* RTP sequence is 16-bit; widen against last using the
                 * AVTP-style 8-bit handling doesn't apply. Treat it as a
                 * 32-bit value; the delta compare below handles wrap. */
                seq = (uint32_t)rseq;
            } else if (transport == TRANSPORT_AVTP) {
                if ((size_t)n < hdr_off + AVTP_HDR_LEN) { dropped++; goto check_feedback; }
                const struct avtp_aaf_hdr *ah =
                    (const struct avtp_aaf_hdr *)(buf + hdr_off);
                uint8_t  af, ans, abd, aseq;
                uint16_t acpf, asdl;
                uint64_t asid;
                uint32_t ats;
                if (avtp_aaf_hdr_parse(ah, &asid, &aseq, &ats,
                                       &af, &ans, &acpf, &abd, &asdl) < 0) {
                    dropped++; goto check_feedback;
                }
                if (af != AAF_FORMAT_INT24 ||
                    ans != avtp_nsr_code  ||
                    abd != 24             ||
                    acpf != channels) {
                    dropped++; goto check_feedback;
                }
                payload_bytes = asdl;
                if ((size_t)n < hdr_off + AVTP_HDR_LEN + payload_bytes) {
                    dropped++; goto check_feedback;
                }
                size_t per_sample = (size_t)channels * bytes_per_sample;
                if (per_sample == 0 || payload_bytes % per_sample != 0) {
                    dropped++; goto check_feedback;
                }
                frames = payload_bytes / per_sample;
                payload_p = buf + hdr_off + AVTP_HDR_LEN;
                /* AAF samples are big-endian on the wire; ALSA wants LE. */
                avtp_swap24_inplace(payload_p, frames * (size_t)channels);
                /* AVTP only carries an 8-bit sequence; widen against last. */
                seq = (uint32_t)aseq;
            } else {
                if ((size_t)n < hdr_off + AOE_HDR_LEN) { dropped++; goto check_feedback; }
                const struct aoe_hdr *hdr =
                    (const struct aoe_hdr *)(buf + hdr_off);
                if (!aoe_hdr_valid(hdr) ||
                    hdr->format != expected_format_code ||
                    hdr->channel_count != channels) {
                    dropped++; goto check_feedback;
                }
                frames = hdr->payload_count;
                payload_bytes = frames * (size_t)channels * bytes_per_sample;
                if ((size_t)n < hdr_off + AOE_HDR_LEN + payload_bytes) {
                    dropped++; goto check_feedback;
                }
                seq = ntohl(hdr->sequence);
                payload_p = buf + hdr_off + AOE_HDR_LEN;
            }

            if (have_seq) {
                /* AVTP wraps every 256 packets, RTP every 65536, AOE every
                 * 2^32. Use a width-appropriate signed-difference compare. */
                if (transport == TRANSPORT_AVTP) {
                    int8_t d8 = (int8_t)((uint8_t)seq - (uint8_t)last_seq);
                    if (d8 > 1) lost += (uint64_t)(d8 - 1);
                } else if (transport == TRANSPORT_RTP) {
                    int16_t d16 = (int16_t)((uint16_t)seq - (uint16_t)last_seq);
                    if (d16 > 1) lost += (uint64_t)(d16 - 1);
                } else {
                    int32_t delta = (int32_t)(seq - last_seq);
                    if (delta > 1) lost += (uint64_t)(delta - 1);
                }
            }
            last_seq = seq;
            have_seq = 1;

            if (!have_talker) {
                /* Prefer the ACMP-announced talker MAC over learning from
                 * the first frame: Hive's Connect should steer us, not the
                 * first arriving packet (which might be a stray from
                 * another talker on the same segment). IP / RTP modes
                 * learn from src_addr because AVDECC is L2/AVTP-only. */
                int used_avdecc = 0;
                if (transport != TRANSPORT_IP && transport != TRANSPORT_RTP) {
                    pthread_mutex_lock(&g_avdecc_mu);
                    if (g_avdecc_peer_valid) {
                        memcpy(talker_mac, g_avdecc_peer_mac, 6);
                        used_avdecc = 1;
                    }
                    pthread_mutex_unlock(&g_avdecc_mu);
                }
                if (transport == TRANSPORT_IP || transport == TRANSPORT_RTP) {
                    memcpy(&talker_addr, &src_addr, src_addrlen);
                    talker_addr_len = src_addrlen;
                } else if (!used_avdecc) {
                    const struct ether_header *eth = (const struct ether_header *)buf;
                    memcpy(talker_mac, eth->ether_shost, 6);
                }
                if (transport != TRANSPORT_IP && transport != TRANSPORT_RTP) {
                    memcpy(fb_eth->ether_dhost, talker_mac, 6);
                    memcpy(fb_to_ll.sll_addr, talker_mac, 6);
                }
                have_talker = 1;
            }

            /* For DSD formats wider than U8, deinterleave wire bytes into
             * per-channel streams (carrying up to N-1 bytes per channel
             * across packet boundaries), then repack into ALSA's N-byte-per-
             * channel frames, byte-reversing within each group for LE
             * variants. For DSD_U8 and PCM the wire layout already matches
             * what ALSA expects and payload_p is passed through unchanged. */
            const uint8_t *alsa_p = payload_p;
            size_t alsa_frames = frames;
            if (fmt.is_dsd && av.n_bytes > 1) {
                const int wire_per_ch = (int)frames;
                const int total_per_ch = dsd_leftover_per_ch + wire_per_ch;
                const int af = total_per_ch / av.n_bytes;
                const int consumed_per_ch = af * av.n_bytes;
                const int new_leftover = total_per_ch - consumed_per_ch;

                /* Build per-channel linear streams: leftover bytes first,
                 * then deinterleaved wire bytes. */
                for (int c = 0; c < channels; c++) {
                    uint8_t *dst = dsd_per_ch + c * total_per_ch;
                    memcpy(dst,
                           dsd_leftover + c * dsd_leftover_per_ch,
                           (size_t)dsd_leftover_per_ch);
                    for (int i = 0; i < wire_per_ch; i++) {
                        dst[dsd_leftover_per_ch + i] =
                            payload_p[i * channels + c];
                    }
                }

                /* Save the new leftover (bytes beyond the last whole ALSA
                 * frame boundary) for the next packet. */
                for (int c = 0; c < channels; c++) {
                    memcpy(dsd_leftover + c * new_leftover,
                           dsd_per_ch + c * total_per_ch + consumed_per_ch,
                           (size_t)new_leftover);
                }
                dsd_leftover_per_ch = new_leftover;

                /* Pack ALSA output: for each ALSA frame f, for each channel
                 * c, copy av.n_bytes bytes from the per-channel stream,
                 * optionally byte-reversing within the group for _LE. */
                for (int f = 0; f < af; f++) {
                    for (int c = 0; c < channels; c++) {
                        const uint8_t *src =
                            dsd_per_ch + c * total_per_ch + f * av.n_bytes;
                        uint8_t *dst =
                            dsd_out + f * channels * av.n_bytes + c * av.n_bytes;
                        if (av.reverse) {
                            for (int i = 0; i < av.n_bytes; i++) {
                                dst[av.n_bytes - 1 - i] = src[i];
                            }
                        } else {
                            memcpy(dst, src, (size_t)av.n_bytes);
                        }
                    }
                }
                alsa_p = dsd_out;
                alsa_frames = (size_t)af;
            }

            snd_pcm_sframes_t w = 0;
            if (alsa_frames > 0) {
                w = snd_pcm_writei(pcm, alsa_p, alsa_frames);
            }
            if (w == -EPIPE) {
                underruns++;
                snd_pcm_prepare(pcm);
                rate_bootstrapped = 0;
                dsd_leftover_per_ch = 0;  /* drop pending on xrun */
            } else if (w < 0) {
                int r = snd_pcm_recover(pcm, (int)w, 1);
                if (r < 0) {
                    fprintf(stderr, "snd_pcm_recover: %s\n", snd_strerror(r));
                    break;
                }
                rate_bootstrapped = 0;
                dsd_leftover_per_ch = 0;
            } else {
                rx++;
                frames_written_total += (uint64_t)w;
            }
        }

check_feedback:
        /* RTP/AES67 doesn't use AOEther's Mode C feedback — AES67 devices
         * expect PTPv2 for clocking and will discard our 0x88B6 frames.
         * Skip emission entirely to avoid spraying unsolicited packets. */
        if (!feedback_enabled || !have_talker || transport == TRANSPORT_RTP) {
            continue;
        }

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t dt_ns = ts_diff_ns(now, last_fb_ts);
        if (dt_ns < (int64_t)FEEDBACK_PERIOD_MS * 1000000) {
            continue;
        }

        snd_pcm_state_t state = snd_pcm_state(pcm);
        if (state != SND_PCM_STATE_RUNNING) {
            last_fb_ts = now;
            continue;
        }

        snd_pcm_sframes_t delay;
        if (snd_pcm_delay(pcm, &delay) < 0 || delay < 0) {
            last_fb_ts = now;
            continue;
        }
        uint64_t consumed = frames_written_total > (uint64_t)delay
                          ? frames_written_total - (uint64_t)delay
                          : 0;

        if (!rate_bootstrapped) {
            last_consumed = consumed;
            last_fb_ts = now;
            rate_bootstrapped = 1;
            continue;
        }

        double dt_s = (double)dt_ns / 1e9;
        double rate_est_hz = (double)(consumed - last_consumed) / dt_s;

        if (rate_est_hz > 0.8 * rate_hz && rate_est_hz < 1.2 * rate_hz) {
            double spms = rate_est_hz / 1000.0;
            uint32_t q = (uint32_t)(spms * 65536.0 + 0.5);
            ssize_t ss;
            if (transport != TRANSPORT_IP) {
                aoe_c_hdr_build_feedback(fb_hdr_l2, STREAM_ID, fb_seq++, q);
                ss = sendto(ctl_sock, fb_frame, sizeof(fb_frame), 0,
                            (struct sockaddr *)&fb_to_ll, sizeof(fb_to_ll));
            } else {
                aoe_c_hdr_build_feedback(fb_hdr_ip, STREAM_ID, fb_seq++, q);
                ss = sendto(ctl_sock, fb_frame, AOE_C_HDR_LEN, 0,
                            (struct sockaddr *)&talker_addr, talker_addr_len);
            }
            if (ss < 0 && errno != EINTR) {
                perror("sendto(feedback)");
            } else {
                fb_sent++;
            }
        }

        last_consumed = consumed;
        last_fb_ts = now;
    }

    fprintf(stderr,
            "receiver: shutting down; rx=%llu dropped=%llu lost=%llu underruns=%llu fb_sent=%llu\n",
            (unsigned long long)rx,
            (unsigned long long)dropped,
            (unsigned long long)lost,
            (unsigned long long)underruns,
            (unsigned long long)fb_sent);

    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    if (ctl_sock != data_sock) close(ctl_sock);
    close(data_sock);
    aoether_mdns_close(mdns);
    aoether_avdecc_close(avdecc);
    return 0;
}
