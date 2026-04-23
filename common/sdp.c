/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sdp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *encoding_tag(enum sdp_encoding e)
{
    switch (e) {
    case SDP_ENC_L24: return "L24";
    case SDP_ENC_L16: return "L16";
    }
    return "L24";
}

static void format_ptime(char *buf, size_t cap, uint32_t ptime_us)
{
    if (ptime_us % 1000 == 0) {
        snprintf(buf, cap, "%u", ptime_us / 1000);
    } else {
        uint32_t ms_whole = ptime_us / 1000;
        uint32_t ms_frac  = ptime_us % 1000;
        while (ms_frac && ms_frac % 10 == 0)
            ms_frac /= 10;
        snprintf(buf, cap, "%u.%u", ms_whole, ms_frac);
    }
}

int sdp_build(char *out, size_t out_cap, const struct sdp_params *p)
{
    if (!out || out_cap == 0 || !p) return -1;
    if (!p->origin_ip || !p->dest_ip) return -1;
    if (p->channels == 0 || p->sample_rate_hz == 0) return -1;

    char ptime[16];
    format_ptime(ptime, sizeof ptime, p->ptime_us);

    const char *addrtype = (p->family == SDP_ADDR_IP6) ? "IP6" : "IP4";

    char c_line[96];
    if (p->ttl > 0)
        snprintf(c_line, sizeof c_line, "c=IN %s %s/%u\r\n",
                 addrtype, p->dest_ip, (unsigned)p->ttl);
    else
        snprintf(c_line, sizeof c_line, "c=IN %s %s\r\n",
                 addrtype, p->dest_ip);

    const char *refclk_block = "";
    char refclk_buf[128];
    if (p->refclk == SDP_REFCLK_PTP_TRACEABLE) {
        refclk_block =
            "a=ts-refclk:ptp=IEEE1588-2008:traceable\r\n"
            "a=mediaclk:direct=0\r\n";
    } else if (p->refclk == SDP_REFCLK_PTP_GMID && p->gmid_str[0]) {
        snprintf(refclk_buf, sizeof refclk_buf,
                 "a=ts-refclk:ptp=IEEE1588-2008:%s:%u\r\n"
                 "a=mediaclk:direct=0\r\n",
                 p->gmid_str, (unsigned)p->ptp_domain);
        refclk_block = refclk_buf;
    }

    int n = snprintf(out, out_cap,
        "v=0\r\n"
        "o=- %llu %llu IN %s %s\r\n"
        "s=%s\r\n"
        "%s"
        "t=0 0\r\n"
        "a=recvonly\r\n"
        "m=audio %u RTP/AVP %u\r\n"
        "a=rtpmap:%u %s/%u/%u\r\n"
        "a=ptime:%s\r\n"
        "a=source-filter: incl IN %s * %s\r\n"
        "%s",
        (unsigned long long)p->session_id,
        (unsigned long long)p->session_version,
        addrtype, p->origin_ip,
        p->session_name[0] ? p->session_name : "AOEther stream",
        c_line,
        (unsigned)p->port,
        (unsigned)p->payload_type,
        (unsigned)p->payload_type,
        encoding_tag(p->encoding),
        (unsigned)p->sample_rate_hz,
        (unsigned)p->channels,
        ptime,
        addrtype, p->origin_ip,
        refclk_block);

    if (n < 0 || (size_t)n >= out_cap) return -1;
    return n;
}

int sdp_build_bundle(char *out, size_t out_cap,
                     const struct sdp_bundle_params *p)
{
    if (!out || out_cap == 0 || !p) return -1;
    if (!p->origin_ip || !p->media || p->n_media == 0) return -1;

    const char *addrtype = (p->family == SDP_ADDR_IP6) ? "IP6" : "IP4";

    /* Append incrementally; track remaining cap. Returns -1 on overflow. */
    size_t off = 0;
#define APPEND(...)  do {                                               \
        int _w = snprintf(out + off, out_cap - off, __VA_ARGS__);        \
        if (_w < 0 || (size_t)_w >= out_cap - off) return -1;            \
        off += (size_t)_w;                                               \
    } while (0)

    APPEND("v=0\r\n"
           "o=- %llu %llu IN %s %s\r\n"
           "s=%s\r\n"
           "t=0 0\r\n",
           (unsigned long long)p->session_id,
           (unsigned long long)p->session_version,
           addrtype, p->origin_ip,
           p->session_name[0] ? p->session_name : "AOEther stream");

    /* Session-level a=group:LS <mid1> <mid2> ... — only when > 1 media,
     * since grouping a single member is meaningless and some controllers
     * reject it. */
    if (p->n_media > 1) {
        APPEND("a=group:LS");
        for (size_t i = 0; i < p->n_media; i++) {
            APPEND(" %d", p->media[i].mid);
        }
        APPEND("\r\n");
    }

    /* Session-level refclk + mediaclk — applies to every m= media since
     * all substreams share the same PTP grandmaster and media clock. */
    if (p->refclk == SDP_REFCLK_PTP_TRACEABLE) {
        APPEND("a=ts-refclk:ptp=IEEE1588-2008:traceable\r\n"
               "a=mediaclk:direct=0\r\n");
    } else if (p->refclk == SDP_REFCLK_PTP_GMID && p->gmid_str[0]) {
        APPEND("a=ts-refclk:ptp=IEEE1588-2008:%s:%u\r\n"
               "a=mediaclk:direct=0\r\n",
               p->gmid_str, (unsigned)p->ptp_domain);
    }

    APPEND("a=recvonly\r\n");

    /* Per-media section. Each m= block carries its own c= (destinations
     * typically differ per substream) and a=mid for group membership. */
    for (size_t i = 0; i < p->n_media; i++) {
        const struct sdp_media *m = &p->media[i];
        if (!m->dest_ip || m->channels == 0 || m->sample_rate_hz == 0) return -1;

        APPEND("m=audio %u RTP/AVP %u\r\n",
               (unsigned)m->port, (unsigned)m->payload_type);

        if (m->ttl > 0) {
            APPEND("c=IN %s %s/%u\r\n",
                   addrtype, m->dest_ip, (unsigned)m->ttl);
        } else {
            APPEND("c=IN %s %s\r\n", addrtype, m->dest_ip);
        }

        if (p->n_media > 1) {
            APPEND("a=mid:%d\r\n", m->mid);
        }

        APPEND("a=rtpmap:%u %s/%u/%u\r\n",
               (unsigned)m->payload_type,
               encoding_tag(m->encoding),
               (unsigned)m->sample_rate_hz,
               (unsigned)m->channels);

        char ptime[16];
        format_ptime(ptime, sizeof ptime, m->ptime_us);
        APPEND("a=ptime:%s\r\n", ptime);

        APPEND("a=source-filter: incl IN %s * %s\r\n",
               addrtype, p->origin_ip);
    }

#undef APPEND
    return (int)off;
}

/* Small line-walker for sdp_parse. Returns the length of the next line
 * (without CR/LF), advances `*cursor` past the line terminator, and
 * writes the line start via `*line_out`. Returns 0 when there are no more
 * lines. */
static size_t next_line(const char **cursor, const char *end,
                        const char **line_out)
{
    if (*cursor >= end) return 0;
    const char *start = *cursor;
    const char *p = start;
    while (p < end && *p != '\r' && *p != '\n') p++;
    size_t len = (size_t)(p - start);
    while (p < end && (*p == '\r' || *p == '\n')) p++;
    *cursor = p;
    *line_out = start;
    return len;
}

static int starts_with(const char *line, size_t len, const char *prefix)
{
    size_t plen = strlen(prefix);
    if (len < plen) return 0;
    return memcmp(line, prefix, plen) == 0;
}

static void copy_bounded(char *dst, size_t cap, const char *src, size_t len)
{
    if (cap == 0) return;
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

int sdp_parse(const char *text, size_t len, struct sdp_params *p)
{
    if (!text || !p) return -1;
    memset(p, 0, sizeof *p);

    /* Temporaries owned by `p` via static-storage string buffers is not
     * worth it — callers who need origin_ip / dest_ip after parse should
     * wrap with their own copy. For now we only fill scalar fields and
     * leave origin_ip / dest_ip as NULL. */
    static char origin_ip_buf[64];
    static char dest_ip_buf[64];
    origin_ip_buf[0] = dest_ip_buf[0] = '\0';

    const char *cursor = text;
    const char *end = text + len;
    const char *line;
    size_t llen;
    int saw_version = 0;

    while ((llen = next_line(&cursor, end, &line)) > 0) {
        if (starts_with(line, llen, "v=")) {
            saw_version = 1;
            continue;
        }
        if (starts_with(line, llen, "o=")) {
            /* o=- <sid> <sver> IN IP4|IP6 <ip> */
            char tmp[256];
            copy_bounded(tmp, sizeof tmp, line + 2, llen - 2);
            char username[32], nettype[8], addrtype[8], ip[64];
            unsigned long long sid, sver;
            if (sscanf(tmp, "%31s %llu %llu %7s %7s %63s",
                       username, &sid, &sver, nettype, addrtype, ip) == 6) {
                p->session_id = sid;
                p->session_version = sver;
                if (strcmp(addrtype, "IP6") == 0)
                    p->family = SDP_ADDR_IP6;
                else
                    p->family = SDP_ADDR_IP4;
                copy_bounded(origin_ip_buf, sizeof origin_ip_buf, ip, strlen(ip));
                p->origin_ip = origin_ip_buf;
            }
            continue;
        }
        if (starts_with(line, llen, "s=")) {
            copy_bounded(p->session_name, sizeof p->session_name,
                         line + 2, llen - 2);
            continue;
        }
        if (starts_with(line, llen, "c=")) {
            /* c=IN IP4 <ip>[/<ttl>] */
            char tmp[96];
            copy_bounded(tmp, sizeof tmp, line + 2, llen - 2);
            char nettype[8], addrtype[8], addr[96];
            if (sscanf(tmp, "%7s %7s %95s", nettype, addrtype, addr) == 3) {
                char *slash = strchr(addr, '/');
                if (slash) {
                    *slash = '\0';
                    p->ttl = (uint8_t)atoi(slash + 1);
                }
                copy_bounded(dest_ip_buf, sizeof dest_ip_buf, addr, strlen(addr));
                p->dest_ip = dest_ip_buf;
            }
            continue;
        }
        if (starts_with(line, llen, "m=audio ")) {
            char tmp[96];
            copy_bounded(tmp, sizeof tmp, line + 8, llen - 8);
            unsigned port, pt;
            char proto[16];
            if (sscanf(tmp, "%u %15s %u", &port, proto, &pt) == 3) {
                p->port = (uint16_t)port;
                p->payload_type = (uint8_t)pt;
            }
            continue;
        }
        if (starts_with(line, llen, "a=rtpmap:")) {
            char tmp[96];
            copy_bounded(tmp, sizeof tmp, line + 9, llen - 9);
            unsigned pt;
            char enc[16];
            unsigned rate, chans;
            if (sscanf(tmp, "%u %15[^/]/%u/%u", &pt, enc, &rate, &chans) == 4) {
                p->sample_rate_hz = rate;
                p->channels = (uint8_t)chans;
                if (strcmp(enc, "L16") == 0) p->encoding = SDP_ENC_L16;
                else p->encoding = SDP_ENC_L24;
            }
            continue;
        }
        if (starts_with(line, llen, "a=ptime:")) {
            char tmp[32];
            copy_bounded(tmp, sizeof tmp, line + 8, llen - 8);
            double ms = atof(tmp);
            p->ptime_us = (uint32_t)(ms * 1000.0 + 0.5);
            continue;
        }
        if (starts_with(line, llen, "a=ts-refclk:ptp=")) {
            /* Two common forms:
             *   a=ts-refclk:ptp=IEEE1588-2008:traceable
             *   a=ts-refclk:ptp=IEEE1588-2008:<gmid>:<domain> */
            const char *rest = line + strlen("a=ts-refclk:ptp=");
            size_t rlen = llen - strlen("a=ts-refclk:ptp=");
            char tmp[128];
            copy_bounded(tmp, sizeof tmp, rest, rlen);
            char profile[32], id_or_flag[48];
            unsigned domain;
            int got = sscanf(tmp, "%31[^:]:%47[^:]:%u",
                             profile, id_or_flag, &domain);
            if (got == 2 && strcmp(id_or_flag, "traceable") == 0) {
                p->refclk = SDP_REFCLK_PTP_TRACEABLE;
            } else if (got == 3) {
                p->refclk = SDP_REFCLK_PTP_GMID;
                strncpy(p->gmid_str, id_or_flag, sizeof p->gmid_str - 1);
                p->gmid_str[sizeof p->gmid_str - 1] = '\0';
                p->ptp_domain = (uint8_t)domain;
            } else {
                p->refclk = SDP_REFCLK_PTP_TRACEABLE;
            }
            continue;
        }
    }

    if (!saw_version) return -1;
    return 0;
}

int sdp_parse_bundle(const char *text, size_t len,
                     struct sdp_params *session_out,
                     struct sdp_media_parsed *media_out,
                     size_t max_media,
                     size_t *n_media_out)
{
    if (!text || !session_out || !media_out || !n_media_out || max_media == 0)
        return -1;

    memset(session_out, 0, sizeof *session_out);
    memset(media_out, 0, max_media * sizeof *media_out);
    *n_media_out = 0;

    /* Shared static buffers for session-level addrs (same pattern as
     * sdp_parse). Callers needing stable copies should memcpy out. */
    static char origin_ip_buf[64];
    origin_ip_buf[0] = '\0';

    const char *cursor = text;
    const char *end = text + len;
    const char *line;
    size_t llen;

    int saw_version = 0;
    struct sdp_media_parsed *cur_media = NULL;  /* NULL until first m= */
    size_t n_media = 0;

    while ((llen = next_line(&cursor, end, &line)) > 0) {
        if (starts_with(line, llen, "v=")) {
            saw_version = 1;
            continue;
        }
        if (starts_with(line, llen, "o=")) {
            char tmp[256];
            copy_bounded(tmp, sizeof tmp, line + 2, llen - 2);
            char username[32], nettype[8], addrtype[8], ip[64];
            unsigned long long sid, sver;
            if (sscanf(tmp, "%31s %llu %llu %7s %7s %63s",
                       username, &sid, &sver, nettype, addrtype, ip) == 6) {
                session_out->session_id = sid;
                session_out->session_version = sver;
                session_out->family = (strcmp(addrtype, "IP6") == 0)
                                    ? SDP_ADDR_IP6 : SDP_ADDR_IP4;
                copy_bounded(origin_ip_buf, sizeof origin_ip_buf,
                             ip, strlen(ip));
                session_out->origin_ip = origin_ip_buf;
            }
            continue;
        }
        if (starts_with(line, llen, "s=")) {
            copy_bounded(session_out->session_name,
                         sizeof session_out->session_name,
                         line + 2, llen - 2);
            continue;
        }
        if (starts_with(line, llen, "a=ts-refclk:ptp=")) {
            const char *rest = line + strlen("a=ts-refclk:ptp=");
            size_t rlen = llen - strlen("a=ts-refclk:ptp=");
            char tmp[128];
            copy_bounded(tmp, sizeof tmp, rest, rlen);
            char profile[32], id_or_flag[48];
            unsigned domain;
            int got = sscanf(tmp, "%31[^:]:%47[^:]:%u",
                             profile, id_or_flag, &domain);
            if (got == 2 && strcmp(id_or_flag, "traceable") == 0) {
                session_out->refclk = SDP_REFCLK_PTP_TRACEABLE;
            } else if (got == 3) {
                session_out->refclk = SDP_REFCLK_PTP_GMID;
                strncpy(session_out->gmid_str, id_or_flag,
                        sizeof session_out->gmid_str - 1);
                session_out->gmid_str[sizeof session_out->gmid_str - 1] = '\0';
                session_out->ptp_domain = (uint8_t)domain;
            } else {
                session_out->refclk = SDP_REFCLK_PTP_TRACEABLE;
            }
            continue;
        }

        /* m=audio opens a new media section. */
        if (starts_with(line, llen, "m=audio ")) {
            if (n_media >= max_media) return -1;
            cur_media = &media_out[n_media++];
            memset(cur_media, 0, sizeof *cur_media);
            char tmp[96];
            copy_bounded(tmp, sizeof tmp, line + 8, llen - 8);
            unsigned port, pt;
            char proto[16];
            if (sscanf(tmp, "%u %15s %u", &port, proto, &pt) == 3) {
                cur_media->port = (uint16_t)port;
                cur_media->payload_type = (uint8_t)pt;
            }
            continue;
        }

        /* c= may appear at session OR media level. In bundled SDP we
         * expect per-media c=; populate the current media if one is
         * open, otherwise ignore (session-level c= doesn't carry per-
         * substream dest for our purposes). */
        if (starts_with(line, llen, "c=") && cur_media) {
            char tmp[96];
            copy_bounded(tmp, sizeof tmp, line + 2, llen - 2);
            char nettype[8], addrtype[8], addr[96];
            if (sscanf(tmp, "%7s %7s %95s", nettype, addrtype, addr) == 3) {
                char *slash = strchr(addr, '/');
                if (slash) {
                    *slash = '\0';
                    cur_media->ttl = (uint8_t)atoi(slash + 1);
                }
                copy_bounded(cur_media->dest_ip,
                             sizeof cur_media->dest_ip,
                             addr, strlen(addr));
            }
            continue;
        }
        if (starts_with(line, llen, "a=mid:") && cur_media) {
            char tmp[16];
            copy_bounded(tmp, sizeof tmp, line + 6, llen - 6);
            cur_media->mid = atoi(tmp);
            continue;
        }
        if (starts_with(line, llen, "a=rtpmap:") && cur_media) {
            char tmp[96];
            copy_bounded(tmp, sizeof tmp, line + 9, llen - 9);
            unsigned pt;
            char enc[16];
            unsigned rate, chans;
            if (sscanf(tmp, "%u %15[^/]/%u/%u", &pt, enc, &rate, &chans) == 4) {
                cur_media->sample_rate_hz = rate;
                cur_media->channels = (uint8_t)chans;
                if (strcmp(enc, "L16") == 0) cur_media->encoding = SDP_ENC_L16;
                else cur_media->encoding = SDP_ENC_L24;
            }
            continue;
        }
        if (starts_with(line, llen, "a=ptime:") && cur_media) {
            char tmp[32];
            copy_bounded(tmp, sizeof tmp, line + 8, llen - 8);
            double ms = atof(tmp);
            cur_media->ptime_us = (uint32_t)(ms * 1000.0 + 0.5);
            continue;
        }
        /* Other attributes (a=recvonly, a=source-filter, a=group:LS,
         * a=mediaclk) are ignored — they don't drive the receive loop. */
    }

    if (!saw_version || n_media == 0) return -1;
    *n_media_out = n_media;
    return 0;
}
