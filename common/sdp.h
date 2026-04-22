#pragma once

#include <stddef.h>
#include <stdint.h>

/* Minimal AES67-flavoured SDP builder + parser (RFC 4566, AES67 §6).
 *
 * Used by M9 Phase B to describe an RTP/AES67 stream out-of-band: either
 * rendered into a SAP announcement (see common/sap.h) or dumped to stdout
 * so the operator can hand-feed the text to a controller that prefers
 * static session files (Merging ANEMAN, Dante Controller in AES67 mode,
 * etc.).
 *
 * Scope: enough of SDP to describe one audio=... media section with L24
 * or L16 PCM. Not a general-purpose SDP parser. */

#define SDP_MAX_LEN             1024
#define SDP_MAX_SESSION_NAME    64

enum sdp_encoding {
    SDP_ENC_L24 = 0,
    SDP_ENC_L16 = 1,
};

enum sdp_refclk {
    /* No a=ts-refclk / a=mediaclk lines. Suitable for free-run talkers
     * before M9 Phase C lands PTPv2 discipline. */
    SDP_REFCLK_NONE = 0,
    /* a=ts-refclk:ptp=IEEE1588-2008:traceable
     * a=mediaclk:direct=0
     * Claims PTP-disciplined timing without pinning to a specific
     * grandmaster identity. AES67 permits this when the talker doesn't
     * know the GM's clock identity yet (linuxptp exposes it but it's
     * fiddly to read cross-process). */
    SDP_REFCLK_PTP_TRACEABLE = 1,
};

struct sdp_params {
    /* Originator's IPv4 source address in dotted-quad, e.g. "192.168.1.100".
     * Used for o= and a=source-filter:. */
    const char *origin_ip;

    /* Destination IPv4 address — typically a multicast group such as
     * "239.69.1.10". Used for c= and (via SAP) as the target of the
     * session. */
    const char *dest_ip;

    /* UDP port (typically 5004). */
    uint16_t    port;

    /* Multicast TTL advertised in c=IN IP4 .../ttl. 32 matches AES67
     * default. Pass 0 to omit the /ttl suffix (unicast). */
    uint8_t     ttl;

    /* Session name: shown in Dante Controller, ANEMAN, etc. ASCII only. */
    char        session_name[SDP_MAX_SESSION_NAME];

    /* Session ID: stable across re-announces; typically the talker's start
     * time as a Unix epoch. Version increments on any SDP change. */
    uint64_t    session_id;
    uint64_t    session_version;

    /* Stream parameters. */
    enum sdp_encoding encoding;
    uint32_t    sample_rate_hz;   /* 48000, 96000, ... */
    uint8_t     channels;         /* 1..8 for AES67 */
    uint8_t     payload_type;     /* dynamic PT, default 96 for L24 */

    /* PTIME in microseconds: 1000 (1 ms) or 125. Rendered as ms with up
     * to one fractional digit (a=ptime:1 or a=ptime:0.125). */
    uint32_t    ptime_us;

    enum sdp_refclk refclk;
};

/* Render the SDP as a NUL-terminated text block into `out`. Returns the
 * number of bytes written (not counting the trailing NUL) on success, or
 * -1 if the buffer is too small. */
int sdp_build(char *out, size_t out_cap, const struct sdp_params *p);

/* Best-effort parse of an AES67-shaped SDP into `p`. Populates only the
 * fields that the minimal builder emits — unknown attributes are skipped.
 * Returns 0 on success, -1 on unparseable input. */
int sdp_parse(const char *text, size_t len, struct sdp_params *p);
