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

    char c_line[96];
    if (p->ttl > 0)
        snprintf(c_line, sizeof c_line, "c=IN IP4 %s/%u\r\n",
                 p->dest_ip, (unsigned)p->ttl);
    else
        snprintf(c_line, sizeof c_line, "c=IN IP4 %s\r\n", p->dest_ip);

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
        "o=- %llu %llu IN IP4 %s\r\n"
        "s=%s\r\n"
        "%s"
        "t=0 0\r\n"
        "a=recvonly\r\n"
        "m=audio %u RTP/AVP %u\r\n"
        "a=rtpmap:%u %s/%u/%u\r\n"
        "a=ptime:%s\r\n"
        "a=source-filter: incl IN IP4 * %s\r\n"
        "%s",
        (unsigned long long)p->session_id,
        (unsigned long long)p->session_version,
        p->origin_ip,
        p->session_name[0] ? p->session_name : "AOEther stream",
        c_line,
        (unsigned)p->port,
        (unsigned)p->payload_type,
        (unsigned)p->payload_type,
        encoding_tag(p->encoding),
        (unsigned)p->sample_rate_hz,
        (unsigned)p->channels,
        ptime,
        p->origin_ip,
        refclk_block);

    if (n < 0 || (size_t)n >= out_cap) return -1;
    return n;
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
            /* o=- <sid> <sver> IN IP4 <ip> */
            char tmp[256];
            copy_bounded(tmp, sizeof tmp, line + 2, llen - 2);
            char username[32], nettype[8], addrtype[8], ip[64];
            unsigned long long sid, sver;
            if (sscanf(tmp, "%31s %llu %llu %7s %7s %63s",
                       username, &sid, &sver, nettype, addrtype, ip) == 6) {
                p->session_id = sid;
                p->session_version = sver;
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
