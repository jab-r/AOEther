#include "packet.h"

#include <alsa/asoundlib.h>
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>
#include <net/if.h>
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

/* M1: hardcoded stream format. See docs/design.md §"M1 detailed plan". */
#define STREAM_ID         0x0001
#define SAMPLE_RATE_HZ    48000
#define CHANNELS          2
#define BYTES_PER_SAMPLE  3

/* Generous M1 latency: Mode C corrects a little ppm of drift slowly; the
 * buffer also absorbs timerfd jitter on the talker. See design.md §M1
 * "Known deferred items". */
#define DEFAULT_LATENCY_US    5000
#define FEEDBACK_PERIOD_MS    20
#define POLL_TIMEOUT_MS       FEEDBACK_PERIOD_MS

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s --iface IF --dac hw:CARD=NAME,DEV=0 [options]\n"
        "  --latency-us N   ALSA period latency hint (default %d)\n"
        "  --no-feedback    do not emit Mode C FEEDBACK frames (diagnostic)\n",
        prog, DEFAULT_LATENCY_US);
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
    int latency_us = DEFAULT_LATENCY_US;
    int feedback_enabled = 1;

    static const struct option opts[] = {
        { "iface",       required_argument, 0, 'i' },
        { "dac",         required_argument, 0, 'd' },
        { "latency-us",  required_argument, 0, 'l' },
        { "no-feedback", no_argument,       0, 'n' },
        { "help",        no_argument,       0, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:d:l:nh", opts, NULL)) != -1) {
        switch (c) {
        case 'i': iface = optarg; break;
        case 'd': dac = optarg; break;
        case 'l': latency_us = atoi(optarg); break;
        case 'n': feedback_enabled = 0; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (!iface || !dac) {
        usage(argv[0]);
        return 2;
    }

    /* Data socket: receives audio frames on EtherType 0x88B5. */
    int data_sock = socket(AF_PACKET, SOCK_RAW, htons(AOE_ETHERTYPE));
    if (data_sock < 0) {
        perror("socket(AF_PACKET, SOCK_RAW) data");
        fprintf(stderr, "receiver: raw sockets need CAP_NET_RAW (try sudo)\n");
        return 1;
    }
    int ifindex;
    uint8_t my_mac[6];
    if (iface_lookup(data_sock, iface, &ifindex, my_mac) < 0) {
        return 1;
    }
    struct sockaddr_ll bind_ll = {
        .sll_family = AF_PACKET,
        .sll_protocol = htons(AOE_ETHERTYPE),
        .sll_ifindex = ifindex,
    };
    if (bind(data_sock, (struct sockaddr *)&bind_ll, sizeof(bind_ll)) < 0) {
        perror("bind(data_sock)");
        return 1;
    }

    /* Control socket: sends FEEDBACK frames on EtherType 0x88B6.
     * Bound with the control protocol so TX uses the right sll_protocol. */
    int ctl_sock = socket(AF_PACKET, SOCK_RAW, htons(AOE_C_ETHERTYPE));
    if (ctl_sock < 0) {
        perror("socket(AF_PACKET, SOCK_RAW) control");
        return 1;
    }

    /* ALSA: open DAC, set the M1-hardcoded format. */
    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, dac, SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_open(%s): %s\n", dac, snd_strerror(err));
        return 1;
    }
    err = snd_pcm_set_params(pcm,
                             SND_PCM_FORMAT_S24_3LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             CHANNELS,
                             SAMPLE_RATE_HZ,
                             0,              /* disable ALSA soft-resample */
                             (unsigned int)latency_us);
    if (err < 0) {
        fprintf(stderr, "snd_pcm_set_params: %s\n", snd_strerror(err));
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr,
            "receiver: iface=%s dac=%s fmt=S24_3LE ch=%d rate=%d latency_us=%d feedback=%s\n",
            iface, dac, CHANNELS, SAMPLE_RATE_HZ, latency_us,
            feedback_enabled ? "on" : "off");

    /* Data-path buffer and counters. */
    uint8_t buf[2048];
    uint32_t last_seq = 0;
    int have_seq = 0;
    uint64_t rx = 0, dropped = 0, lost = 0, underruns = 0;

    /* Mode C state. */
    uint64_t frames_written_total = 0;
    uint64_t last_consumed = 0;
    struct timespec last_fb_ts = { 0, 0 };
    int rate_bootstrapped = 0;

    uint8_t talker_mac[6];
    int have_talker_mac = 0;

    uint16_t fb_seq = 0;
    uint64_t fb_sent = 0;

    /* Pre-built feedback frame template (28 bytes; kernel pads to 60). */
    uint8_t fb_frame[60];
    memset(fb_frame, 0, sizeof(fb_frame));
    struct ether_header *fb_eth = (struct ether_header *)fb_frame;
    struct aoe_c_hdr *fb_hdr =
        (struct aoe_c_hdr *)(fb_frame + sizeof(struct ether_header));
    memcpy(fb_eth->ether_shost, my_mac, 6);
    fb_eth->ether_type = htons(AOE_C_ETHERTYPE);

    struct sockaddr_ll fb_to = {
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
            ssize_t n = recv(data_sock, buf, sizeof(buf), 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("recv");
                break;
            }
            if ((size_t)n < sizeof(struct ether_header) + AOE_HDR_LEN) {
                dropped++;
                goto check_feedback;
            }
            const struct ether_header *eth = (const struct ether_header *)buf;
            const struct aoe_hdr *hdr =
                (const struct aoe_hdr *)(buf + sizeof(struct ether_header));
            if (!aoe_hdr_valid(hdr) ||
                hdr->format != AOE_FMT_PCM_S24LE_3 ||
                hdr->channel_count != CHANNELS) {
                dropped++;
                goto check_feedback;
            }

            size_t frames = hdr->payload_count;
            size_t payload_bytes = frames * CHANNELS * BYTES_PER_SAMPLE;
            if ((size_t)n < sizeof(struct ether_header) + AOE_HDR_LEN + payload_bytes) {
                dropped++;
                goto check_feedback;
            }

            uint32_t seq = ntohl(hdr->sequence);
            if (have_seq) {
                int32_t delta = (int32_t)(seq - last_seq);
                if (delta > 1) lost += (uint64_t)(delta - 1);
            }
            last_seq = seq;
            have_seq = 1;

            if (!have_talker_mac) {
                memcpy(talker_mac, eth->ether_shost, 6);
                memcpy(fb_eth->ether_dhost, talker_mac, 6);
                memcpy(fb_to.sll_addr, talker_mac, 6);
                have_talker_mac = 1;
            }

            const uint8_t *payload = buf + sizeof(struct ether_header) + AOE_HDR_LEN;
            snd_pcm_sframes_t w = snd_pcm_writei(pcm, payload, frames);
            if (w == -EPIPE) {
                underruns++;
                snd_pcm_prepare(pcm);
                /* Rate estimator must re-seed after xrun. */
                rate_bootstrapped = 0;
            } else if (w < 0) {
                int r = snd_pcm_recover(pcm, (int)w, 1);
                if (r < 0) {
                    fprintf(stderr, "snd_pcm_recover: %s\n", snd_strerror(r));
                    break;
                }
                rate_bootstrapped = 0;
            } else {
                rx++;
                frames_written_total += (uint64_t)w;
            }
        }

check_feedback:
        if (!feedback_enabled || !have_talker_mac) {
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
        double rate_hz = (double)(consumed - last_consumed) / dt_s;

        /* Sanity band: ±20% of nominal, generous to admit startup transients. */
        if (rate_hz > 0.8 * SAMPLE_RATE_HZ && rate_hz < 1.2 * SAMPLE_RATE_HZ) {
            double spms = rate_hz / 1000.0;
            uint32_t q = (uint32_t)(spms * 65536.0 + 0.5);
            aoe_c_hdr_build_feedback(fb_hdr, STREAM_ID, fb_seq++, q);
            ssize_t ss = sendto(ctl_sock, fb_frame, sizeof(fb_frame), 0,
                                (struct sockaddr *)&fb_to, sizeof(fb_to));
            if (ss < 0 && errno != EINTR) {
                /* Don't bail on transient ctl-path errors; keep playing audio. */
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
    close(ctl_sock);
    close(data_sock);
    return 0;
}
