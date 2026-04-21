#include "packet.h"

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

/* Stream format. Channels and rate are runtime-configured from M2 on; the
 * sample format is still locked to s24le-3 (other widths arrive later). See
 * docs/design.md §"M2" for the scope of what's parameterizable. */
#define STREAM_ID         0x0001
#define BYTES_PER_SAMPLE  3

/* Defaults match M1's previous hardcoded values. */
#define DEFAULT_CHANNELS      2
#define DEFAULT_RATE_HZ       48000
#define DEFAULT_LATENCY_US    5000
#define FEEDBACK_PERIOD_MS    20
#define POLL_TIMEOUT_MS       FEEDBACK_PERIOD_MS

/* Packet RX buffer: largest legal M2 frame is 12 ch × 3 B × 48 samples
 * (192 kHz microframe) = 1728 B plus 30 B header. 4 KiB is plenty. */
#define RX_BUF_BYTES     4096

/* IP/UDP default port (interim; see docs/wire-format.md). */
#define DEFAULT_UDP_PORT  8805

enum transport_mode {
    TRANSPORT_L2 = 0,
    TRANSPORT_IP = 1,
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
        "  --transport l2|ip    transport mode, default l2\n"
        "  --port N             UDP port (IP mode, default %d)\n"
        "  --group IP           multicast group to join (IP mode, optional;\n"
        "                       IPv4 in 224.0.0.0/4 or IPv6 in ff00::/8)\n"
        "  --channels N         stream channel count (1..64, default %d)\n"
        "  --rate HZ            44100 | 48000 | 88200 | 96000 | 176400 | 192000\n"
        "                       (default %d)\n"
        "  --latency-us N       ALSA period latency hint (default %d)\n"
        "  --no-feedback        do not emit Mode C FEEDBACK frames (diagnostic)\n",
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
    int channels = DEFAULT_CHANNELS;
    int rate_hz = DEFAULT_RATE_HZ;
    int latency_us = DEFAULT_LATENCY_US;
    int feedback_enabled = 1;
    enum transport_mode transport = TRANSPORT_L2;
    int udp_port = DEFAULT_UDP_PORT;

    static const struct option opts[] = {
        { "iface",       required_argument, 0, 'i' },
        { "dac",         required_argument, 0, 'd' },
        { "transport",   required_argument, 0, 'T' },
        { "port",        required_argument, 0, 'P' },
        { "group",       required_argument, 0, 'G' },
        { "channels",    required_argument, 0, 'C' },
        { "rate",        required_argument, 0, 'r' },
        { "latency-us",  required_argument, 0, 'l' },
        { "no-feedback", no_argument,       0, 'n' },
        { "help",        no_argument,       0, 'h' },
        { 0, 0, 0, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:d:T:P:G:C:r:l:nh", opts, NULL)) != -1) {
        switch (c) {
        case 'i': iface = optarg; break;
        case 'd': dac = optarg; break;
        case 'T':
            if (strcmp(optarg, "l2") == 0) transport = TRANSPORT_L2;
            else if (strcmp(optarg, "ip") == 0) transport = TRANSPORT_IP;
            else { fprintf(stderr, "receiver: --transport must be l2 or ip\n"); return 2; }
            break;
        case 'P': udp_port = atoi(optarg); break;
        case 'G': group_s = optarg; break;
        case 'C': channels = atoi(optarg); break;
        case 'r': rate_hz = atoi(optarg); break;
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
    if (udp_port < 1 || udp_port > 65535) {
        fprintf(stderr, "receiver: --port out of range\n");
        return 2;
    }
    if (transport == TRANSPORT_L2 && group_s) {
        fprintf(stderr, "receiver: --group only makes sense with --transport ip\n");
        return 2;
    }
    if (channels < 1 || channels > 64) {
        fprintf(stderr, "receiver: --channels must be 1..64 (got %d)\n", channels);
        return 2;
    }
    if (!rate_supported(rate_hz)) {
        fprintf(stderr, "receiver: unsupported --rate %d\n", rate_hz);
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

    if (transport == TRANSPORT_L2) {
        data_sock = socket(AF_PACKET, SOCK_RAW, htons(AOE_ETHERTYPE));
        if (data_sock < 0) {
            perror("socket(AF_PACKET, SOCK_RAW) data");
            fprintf(stderr, "receiver: raw sockets need CAP_NET_RAW (try sudo)\n");
            return 1;
        }
        if (iface_lookup(data_sock, iface, &ifindex, my_mac) < 0) return 1;
        struct sockaddr_ll bind_ll = {
            .sll_family = AF_PACKET,
            .sll_protocol = htons(AOE_ETHERTYPE),
            .sll_ifindex = ifindex,
        };
        if (bind(data_sock, (struct sockaddr *)&bind_ll, sizeof(bind_ll)) < 0) {
            perror("bind(data_sock)");
            return 1;
        }
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
    err = snd_pcm_set_params(pcm,
                             SND_PCM_FORMAT_S24_3LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             (unsigned int)channels,
                             (unsigned int)rate_hz,
                             0,              /* disable ALSA soft-resample */
                             (unsigned int)latency_us);
    if (err < 0) {
        fprintf(stderr,
                "snd_pcm_set_params (ch=%d rate=%d S24_3LE): %s\n"
                "  (DAC must natively support this configuration; "
                "AOEther never resamples.)\n",
                channels, rate_hz, snd_strerror(err));
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (transport == TRANSPORT_L2) {
        fprintf(stderr,
                "receiver: transport=l2 iface=%s dac=%s fmt=S24_3LE ch=%d rate=%d latency_us=%d feedback=%s\n",
                iface, dac, channels, rate_hz, latency_us,
                feedback_enabled ? "on" : "off");
    } else {
        fprintf(stderr,
                "receiver: transport=ip family=%s %s port=%d%s%s\n"
                "          iface=%s dac=%s fmt=S24_3LE ch=%d rate=%d latency_us=%d feedback=%s\n",
                group_family == AF_INET6 ? "v6" : "v4",
                use_multicast ? "multicast" : "unicast",
                udp_port,
                group_s ? " group=" : "",
                group_s ? group_s : "",
                iface, dac, channels, rate_hz, latency_us,
                feedback_enabled ? "on" : "off");
    }

    /* Data-path buffer and counters. */
    uint8_t buf[RX_BUF_BYTES];
    uint32_t last_seq = 0;
    int have_seq = 0;
    uint64_t rx = 0, dropped = 0, lost = 0, underruns = 0;

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
    if (transport == TRANSPORT_L2) {
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
            if (transport == TRANSPORT_L2) {
                n = recv(data_sock, buf, sizeof(buf), 0);
            } else {
                n = recvfrom(data_sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src_addr, &src_addrlen);
            }
            if (n < 0) {
                if (errno == EINTR) continue;
                perror("recv");
                break;
            }

            size_t hdr_off = (transport == TRANSPORT_L2) ? sizeof(struct ether_header) : 0;
            if ((size_t)n < hdr_off + AOE_HDR_LEN) {
                dropped++;
                goto check_feedback;
            }
            const struct aoe_hdr *hdr =
                (const struct aoe_hdr *)(buf + hdr_off);
            if (!aoe_hdr_valid(hdr) ||
                hdr->format != AOE_FMT_PCM_S24LE_3 ||
                hdr->channel_count != channels) {
                dropped++;
                goto check_feedback;
            }

            size_t frames = hdr->payload_count;
            size_t payload_bytes = (size_t)frames * (size_t)channels * BYTES_PER_SAMPLE;
            if ((size_t)n < hdr_off + AOE_HDR_LEN + payload_bytes) {
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

            if (!have_talker) {
                if (transport == TRANSPORT_L2) {
                    const struct ether_header *eth = (const struct ether_header *)buf;
                    memcpy(talker_mac, eth->ether_shost, 6);
                    memcpy(fb_eth->ether_dhost, talker_mac, 6);
                    memcpy(fb_to_ll.sll_addr, talker_mac, 6);
                } else {
                    memcpy(&talker_addr, &src_addr, src_addrlen);
                    talker_addr_len = src_addrlen;
                }
                have_talker = 1;
            }

            const uint8_t *payload = buf + hdr_off + AOE_HDR_LEN;
            snd_pcm_sframes_t w = snd_pcm_writei(pcm, payload, frames);
            if (w == -EPIPE) {
                underruns++;
                snd_pcm_prepare(pcm);
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
        if (!feedback_enabled || !have_talker) {
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
            if (transport == TRANSPORT_L2) {
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
    return 0;
}
