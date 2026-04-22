#pragma once

#include <stddef.h>
#include <stdint.h>

/* Session Announcement Protocol (RFC 2974) — just enough to emit AES67
 * session announcements from the talker and (optionally) passively parse
 * them on the receiver.
 *
 * Wire layout for the packets we emit (no auth, no encryption, no
 * compression). The originating-source field is 4 bytes when A=0 (IPv4)
 * and 16 bytes when A=1 (IPv6); both forms are supported here.
 *
 *    0                   1                   2                   3
 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   | V=1 |A|R|T|E|C|   auth len    |         msg id hash           |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |               originating source (4 B v4 / 16 B v6)           |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   | "application/sdp\0" (16 bytes)                                |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   | SDP payload (variable)                                        |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * The `T` bit selects announce (0) or deletion (1); on graceful exit the
 * talker sends one deletion packet so controllers drop the session
 * promptly rather than waiting out a ~60 s timeout.
 *
 * IPv4: announcements go to 239.255.255.255:9875 (AES67 convention).
 * IPv6: announcements go to ff0e::2:7ffe:9875 (RFC 2974 §3, global scope).
 * `ttl` follows AES67 convention (32). */

#define SAP_PORT                9875
#define SAP_IPV4_ADDR_STR       "239.255.255.255"
#define SAP_IPV6_ADDR_STR       "ff0e::2:7ffe"
#define SAP_MIME                "application/sdp"
#define SAP_MIME_LEN            16   /* includes trailing NUL */
#define SAP_HDR_LEN_V4          8    /* flags+auth + msg-id + 4-byte origin */
#define SAP_HDR_LEN_V6          20   /* flags+auth + msg-id + 16-byte origin */
#define SAP_DEFAULT_TTL         32
#define SAP_ANNOUNCE_INTERVAL_S 30

enum sap_kind {
    SAP_ANNOUNCE = 0,
    SAP_DELETION = 1,
};

/* Address-family-tagged origin field. SAP supports both IPv4 (A=0, 4-byte
 * origin) and IPv6 (A=1, 16-byte origin); pass the matching family. */
struct sap_origin {
    int family;             /* AF_INET or AF_INET6 */
    union {
        uint32_t v4_be;     /* network byte order */
        uint8_t  v6[16];
    } addr;
};

/* Build one SAP packet into `out`. Returns the total byte count written,
 * or -1 if the buffer is too small or the origin family is unsupported.
 * `msg_id_hash` should be stable across re-announces for the same session. */
int sap_build(uint8_t *out, size_t out_cap,
              enum sap_kind kind,
              const struct sap_origin *origin,
              uint16_t msg_id_hash,
              const char *sdp,
              size_t sdp_len);

/* Parse an inbound SAP packet. On success returns 0 and writes pointers
 * into the original buffer for the SDP section (`*sdp_out`, `*sdp_len_out`).
 * `*origin_out` is populated with the family + address from the header.
 * Rejects non-v1 packets, compressed or encrypted variants, and packets
 * whose MIME is not "application/sdp". */
int sap_parse(const uint8_t *buf, size_t len,
              enum sap_kind *kind_out,
              struct sap_origin *origin_out,
              uint16_t *msg_id_hash_out,
              const char **sdp_out,
              size_t *sdp_len_out);

/* Open an AF_INET / AF_INET6 UDP socket bound to egress via `iface_name`
 * (may be NULL), with multicast TTL/hops = 32 and loopback disabled.
 * Returns the fd on success, -1 on failure. The caller supplies the
 * destination sockaddr when calling sendto(). */
int sap_open_tx_socket(int family, const char *iface_name);

/* Open an AF_INET / AF_INET6 UDP socket joined to the SAP multicast group
 * for that family on `iface_name`, ready to recvfrom() announcements.
 * Returns fd or -1. */
int sap_open_rx_socket(int family, const char *iface_name);
