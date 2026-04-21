#include "audio_source.h"
#include "packet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
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

/* Hardcoded M1 stream parameters (see docs/design.md §"M1 detailed plan"). */
#define STREAM_ID             0x0001
#define SAMPLE_RATE_HZ        48000
#define CHANNELS              2
#define BYTES_PER_SAMPLE      3
#define FORMAT_CODE           AOE_FMT_PCM_S24LE_3
#define PACKET_PERIOD_NS      125000L          /* 125 µs = 1 USB microframe */
#define MICROFRAMES_PER_MS    8
#define NOMINAL_SPM           ((double)SAMPLE_RATE_HZ / 1000.0 / MICROFRAMES_PER_MS)  /* 6.0 */

/* Safety clamp on feedback-derived rate: ±1000 ppm of nominal. */
#define RATE_CLAMP_PPM        1000.0

/* Max samples per packet (buffer sizing headroom; nominal 6, drift barely
 * changes it). */
#define MAX_SAMPLES_PER_PACKET 16

/* Talker reverts to nominal after this long without FEEDBACK. */
#define FEEDBACK_STALE_MS     5000

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
        "usage: %s --iface IF --dest-mac AA:BB:CC:DD:EE:FF [options]\n"
        "  --source testtone|wav|alsa   default: testtone\n"
        "  --file   PATH                WAV file, required with --source wav\n"
        "  --capture hw:CARD=...        ALSA capture device, required with --source alsa\n"
        "\n"
        "M1 stream is hardcoded 48 kHz / 2ch / s24le-3. Sources must match.\n"
        "For music playback, point --capture at one half of a snd-aloop pair\n"
        "and route Roon/UPnP/AirPlay/PipeWire at the other half; see\n"
        "docs/recipe-*.md.\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *iface = NULL;
    const char *dest_s = NULL;
    const char *source = "testtone";
    const char *wav_path = NULL;
    const char *capture_pcm = NULL;

    static const struct option opts[] = {
        { "iface",    required_argument, 0, 'i' },
        { "dest-mac", required_argument, 0, 'd' },
        { "source",   required_argument, 0, 's' },
        { "file",     required_argument, 0, 'f' },
        { "capture",  required_argument, 0, 'c' },
        { "help",     no_argument,       0, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:d:s:f:c:h", opts, NULL)) != -1) {
        switch (c) {
        case 'i': iface = optarg; break;
        case 'd': dest_s = optarg; break;
        case 's': source = optarg; break;
        case 'f': wav_path = optarg; break;
        case 'c': capture_pcm = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (!iface || !dest_s) {
        usage(argv[0]);
        return 2;
    }

    uint8_t dest_mac[6];
    if (parse_mac(dest_s, dest_mac) < 0) {
        fprintf(stderr, "talker: bad --dest-mac\n");
        return 2;
    }

    struct audio_source *src = NULL;
    if (strcmp(source, "testtone") == 0) {
        src = audio_source_test_open(CHANNELS, SAMPLE_RATE_HZ, BYTES_PER_SAMPLE);
    } else if (strcmp(source, "wav") == 0) {
        if (!wav_path) {
            fprintf(stderr, "talker: --file required with --source wav\n");
            return 2;
        }
        src = audio_source_wav_open(wav_path);
    } else if (strcmp(source, "alsa") == 0) {
        if (!capture_pcm) {
            fprintf(stderr, "talker: --capture hw:... required with --source alsa\n");
            return 2;
        }
        src = audio_source_alsa_open(capture_pcm, CHANNELS, SAMPLE_RATE_HZ);
    } else {
        fprintf(stderr, "talker: unknown --source %s\n", source);
        return 2;
    }
    if (!src) return 1;

    /* Data socket: sends audio on EtherType 0x88B5. */
    int data_sock = socket(AF_PACKET, SOCK_RAW, htons(AOE_ETHERTYPE));
    if (data_sock < 0) {
        perror("socket(AF_PACKET, SOCK_RAW) data");
        fprintf(stderr, "talker: raw sockets need CAP_NET_RAW (try sudo)\n");
        return 1;
    }

    int ifindex;
    uint8_t src_mac[6];
    if (iface_lookup(data_sock, iface, &ifindex, src_mac) < 0) return 1;

    struct sockaddr_ll data_to = {
        .sll_family = AF_PACKET,
        .sll_protocol = htons(AOE_ETHERTYPE),
        .sll_ifindex = ifindex,
        .sll_halen = 6,
    };
    memcpy(data_to.sll_addr, dest_mac, 6);

    /* Feedback socket: receives FEEDBACK on EtherType 0x88B6. */
    int fb_sock = socket(AF_PACKET, SOCK_RAW, htons(AOE_C_ETHERTYPE));
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

    const size_t payload_cap = (size_t)CHANNELS * BYTES_PER_SAMPLE * MAX_SAMPLES_PER_PACKET;
    const size_t frame_cap = sizeof(struct ether_header) + AOE_HDR_LEN + payload_cap;
    uint8_t *frame = calloc(1, frame_cap);
    if (!frame) return 1;

    struct ether_header *eth = (struct ether_header *)frame;
    memcpy(eth->ether_dhost, dest_mac, 6);
    memcpy(eth->ether_shost, src_mac, 6);
    eth->ether_type = htons(AOE_ETHERTYPE);

    struct aoe_hdr *hdr = (struct aoe_hdr *)(frame + sizeof(struct ether_header));
    uint8_t *payload = frame + sizeof(struct ether_header) + AOE_HDR_LEN;

    fprintf(stderr,
            "talker: iface=%s ifindex=%d\n"
            "        src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x\n"
            "        fmt=PCM_s24le-3 ch=%d rate=%d pps=8000 nominal spp=%.1f feedback=on\n",
            iface, ifindex,
            src_mac[0], src_mac[1], src_mac[2], src_mac[3], src_mac[4], src_mac[5],
            dest_mac[0], dest_mac[1], dest_mac[2], dest_mac[3], dest_mac[4], dest_mac[5],
            CHANNELS, SAMPLE_RATE_HZ, NOMINAL_SPM);

    /* Mode C talker state: current target samples-per-microframe, fractional
     * accumulator, and bookkeeping for stale-feedback fallback. */
    double samples_per_microframe = NOMINAL_SPM;
    double sample_accum = 0.0;
    struct timespec last_fb_rx_ts = { 0, 0 };
    int have_fb = 0;
    uint16_t last_fb_seq = 0;
    int have_fb_seq = 0;

    uint32_t seq = 0;
    uint64_t late_wakeups = 0;
    uint64_t fb_rx = 0, fb_ignored = 0;

    while (!g_stop) {
        /* 1. Drain any pending feedback frames (non-blocking). */
        for (;;) {
            uint8_t fb_buf[128];
            ssize_t fn = recv(fb_sock, fb_buf, sizeof(fb_buf), 0);
            if (fn < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                if (errno == EINTR) continue;
                perror("recv(fb)");
                break;
            }
            if ((size_t)fn < sizeof(struct ether_header) + AOE_C_HDR_LEN) continue;
            const struct aoe_c_hdr *fh =
                (const struct aoe_c_hdr *)(fb_buf + sizeof(struct ether_header));
            if (!aoe_c_hdr_valid(fh)) continue;
            if (fh->frame_type != AOE_C_TYPE_FEEDBACK) continue;
            if (ntohs(fh->stream_id) != STREAM_ID) continue;

            uint16_t s = ntohs(fh->sequence);
            if (have_fb_seq) {
                /* Reject stale (out-of-order) feedback to avoid rate whiplash. */
                int16_t d = (int16_t)(s - last_fb_seq);
                if (d <= 0) { fb_ignored++; continue; }
            }
            last_fb_seq = s;
            have_fb_seq = 1;

            uint32_t q = ntohl(fh->value);
            double spms = (double)q / 65536.0;
            double new_spm = spms / (double)MICROFRAMES_PER_MS;

            double max_spm = NOMINAL_SPM * (1.0 + RATE_CLAMP_PPM * 1e-6);
            double min_spm = NOMINAL_SPM * (1.0 - RATE_CLAMP_PPM * 1e-6);
            if (new_spm < min_spm || new_spm > max_spm) {
                fb_ignored++;
                continue;
            }

            samples_per_microframe = new_spm;
            clock_gettime(CLOCK_MONOTONIC, &last_fb_rx_ts);
            have_fb = 1;
            fb_rx++;
        }

        /* 2. Stale-feedback fallback: revert to nominal after FEEDBACK_STALE_MS. */
        if (have_fb) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (ts_diff_ms(now, last_fb_rx_ts) > FEEDBACK_STALE_MS) {
                samples_per_microframe = NOMINAL_SPM;
                have_fb = 0;
            }
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

        /* 4. Emit `ticks` packets. Each packet's payload_count is the integer
         *    part of the accumulator; residual carries to the next packet. */
        for (uint64_t i = 0; i < ticks && !g_stop; i++) {
            sample_accum += samples_per_microframe;
            int pc = (int)sample_accum;
            if (pc < 1) pc = 1;
            if (pc > MAX_SAMPLES_PER_PACKET) pc = MAX_SAMPLES_PER_PACKET;
            sample_accum -= pc;

            if (src->read(src, payload, (size_t)pc) < 0) {
                g_stop = 1;
                break;
            }
            aoe_hdr_build(hdr, STREAM_ID, seq, 0,
                          CHANNELS, FORMAT_CODE, (uint8_t)pc,
                          AOE_FLAG_LAST_IN_GROUP);

            size_t frame_len =
                sizeof(struct ether_header) + AOE_HDR_LEN
                + (size_t)pc * CHANNELS * BYTES_PER_SAMPLE;

            ssize_t sent = sendto(data_sock, frame, frame_len, 0,
                                  (struct sockaddr *)&data_to, sizeof(data_to));
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
    close(fb_sock);
    close(data_sock);
    free(frame);
    return 0;
}
