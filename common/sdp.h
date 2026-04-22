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

/* Address family used in c=, o=, and a=source-filter lines. RFC 4566
 * spells these as "IP4" / "IP6". `c=IN IP6 <addr>/<hops>` reuses the
 * `ttl` field below as a hop limit (same wire syntax, different name). */
enum sdp_addr_family {
    SDP_ADDR_IP4 = 0,
    SDP_ADDR_IP6 = 1,
};

enum sdp_refclk {
    /* No a=ts-refclk / a=mediaclk lines. Suitable for free-run talkers
     * before M9 Phase C lands PTPv2 discipline. */
    SDP_REFCLK_NONE = 0,
    /* a=ts-refclk:ptp=IEEE1588-2008:traceable
     * a=mediaclk:direct=0
     * Claims PTP-disciplined timing without pinning to a specific
     * grandmaster identity. AES67 permits this when the talker can't
     * read the current grandmaster clock identity (e.g., ptp4l isn't
     * reachable, or pmc isn't installed). */
    SDP_REFCLK_PTP_TRACEABLE = 1,
    /* a=ts-refclk:ptp=IEEE1588-2008:<gmid>:<domain>
     * a=mediaclk:direct=0
     * Pins the specific grandmaster identity and PTP domain. Strict
     * AES67 controllers (some Dante Domain Manager configs, Ravenna
     * endpoints) require this before they'll lock. The gmid is read
     * from pmc; see common/ptp_pmc.c. */
    SDP_REFCLK_PTP_GMID      = 2,
};

struct sdp_params {
    /* Address family for origin_ip / dest_ip. Both must be the same
     * family — SDP requires the o= and c= addrtype tokens to agree. */
    enum sdp_addr_family family;

    /* Originator's source address as a literal: "192.168.1.100" for v4
     * or "fd00::1234" for v6. Used for o= and a=source-filter:. */
    const char *origin_ip;

    /* Destination address — typically a multicast group such as
     * "239.69.1.10" or "ff3e::beef". Used for c= and (via SAP) as the
     * target of the session. */
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

    /* Only consulted when refclk == SDP_REFCLK_PTP_GMID. gmid is the
     * RFC 7273 rendering of the 8-byte grandmaster clock identity
     * ("hh-hh-hh-hh-hh-hh-hh-hh"); domain is the PTP domain number
     * advertised by ptp4l. */
    char    gmid_str[32];
    uint8_t ptp_domain;
};

/* Render the SDP as a NUL-terminated text block into `out`. Returns the
 * number of bytes written (not counting the trailing NUL) on success, or
 * -1 if the buffer is too small. */
int sdp_build(char *out, size_t out_cap, const struct sdp_params *p);

/* Best-effort parse of an AES67-shaped SDP into `p`. Populates only the
 * fields that the minimal builder emits — unknown attributes are skipped.
 * Returns 0 on success, -1 on unparseable input. */
int sdp_parse(const char *text, size_t len, struct sdp_params *p);

/* ------------------------------------------------------------------
 * M10 Phase B — multi-stream bundled SDP (RFC 5888 a=group:LS).
 *
 * AES67 caps a single RTP session at 8 channels. To carry > 8 channels
 * we emit one SDP with N m=audio sections and a session-level
 * a=group:LS line naming each section's a=mid, so a listener that
 * understands RFC 5888 binds all substreams with shared lip-sync
 * semantics. Controllers that ignore a=group:LS still bind each
 * substream independently — the timestamp-alignment invariant that
 * keeps substreams sample-coherent lives on the RTP wire, not in the
 * SDP.
 * ------------------------------------------------------------------ */

/* One m=audio section in a bundled SDP. */
struct sdp_media {
    int         mid;               /* 1-based; rendered as a=mid:<N> */
    const char *dest_ip;           /* per-media destination literal */
    uint16_t    port;
    uint8_t     ttl;               /* 0 omits /ttl (unicast) */
    enum sdp_encoding encoding;
    uint32_t    sample_rate_hz;
    uint8_t     channels;          /* 1..8 (AES67 cap) */
    uint8_t     payload_type;
    uint32_t    ptime_us;
};

struct sdp_bundle_params {
    enum sdp_addr_family family;               /* applies to o= and all c= */
    const char *origin_ip;
    char        session_name[SDP_MAX_SESSION_NAME];
    uint64_t    session_id;
    uint64_t    session_version;
    enum sdp_refclk refclk;                    /* session-level */
    char        gmid_str[32];
    uint8_t     ptp_domain;
    const struct sdp_media *media;             /* at least one entry */
    size_t      n_media;
};

/* Render a bundled SDP. n_media == 1 emits a single-m= SDP (without the
 * a=group:LS line — lip-sync grouping is meaningless with one member);
 * n_media > 1 adds the group line and per-media a=mid tags. Returns
 * the number of bytes written (excluding the trailing NUL), or -1 on
 * buffer-too-small / malformed input. */
int sdp_build_bundle(char *out, size_t out_cap,
                     const struct sdp_bundle_params *p);

/* Per-media decoded form for sdp_parse_bundle. dest_ip points into a
 * parser-owned static buffer that stays valid until the next call —
 * callers that need persistence should copy. */
struct sdp_media_parsed {
    int      mid;                 /* 0 if a=mid was absent */
    char     dest_ip[64];         /* literal from c= (no /ttl suffix) */
    uint8_t  ttl;
    uint16_t port;
    enum sdp_encoding encoding;
    uint32_t sample_rate_hz;
    uint8_t  channels;
    uint8_t  payload_type;
    uint32_t ptime_us;
};

/* Parse a bundled AES67 SDP into session params + up to `max_media`
 * media descriptors. Fills `*n_media_out` with the actual count.
 * Returns 0 on success, -1 on malformed input or when more media
 * sections are present than `max_media`. Missing a=group:LS is
 * tolerated (older AES67 gear may omit it on single-stream sessions);
 * the caller can still bind each m= individually. */
int sdp_parse_bundle(const char *text, size_t len,
                     struct sdp_params *session_out,
                     struct sdp_media_parsed *media_out,
                     size_t max_media,
                     size_t *n_media_out);
