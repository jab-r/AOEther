#pragma once

#include <stddef.h>
#include <stdint.h>

/* Session Announcement Protocol (RFC 2974) — just enough to emit AES67
 * session announcements from the talker and (optionally) passively parse
 * them on the receiver.
 *
 * Wire layout for the packets we emit (IPv4 only — no auth, no encryption,
 * no compression):
 *
 *    0                   1                   2                   3
 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   | V=1 |A|R|T|E|C|   auth len    |         msg id hash           |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                      originating source IPv4                  |
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
 * AES67 uses 239.255.255.255:9875 as the announcement address regardless
 * of the media stream's group; `ttl` follows AES67 convention (32). */

#define SAP_PORT                9875
#define SAP_IPV4_ADDR_STR       "239.255.255.255"
#define SAP_MIME                "application/sdp"
#define SAP_MIME_LEN            16   /* includes trailing NUL */
#define SAP_HDR_LEN             8    /* flags+auth + msg-id + source */
#define SAP_DEFAULT_TTL         32
#define SAP_ANNOUNCE_INTERVAL_S 30

enum sap_kind {
    SAP_ANNOUNCE = 0,
    SAP_DELETION = 1,
};

/* Build one SAP packet into `out`. Returns the total byte count written,
 * or -1 if the buffer is too small. `origin_ipv4_be` is the originator's
 * IPv4 address in network byte order. `msg_id_hash` should be stable
 * across re-announces for the same session. */
int sap_build(uint8_t *out, size_t out_cap,
              enum sap_kind kind,
              uint32_t origin_ipv4_be,
              uint16_t msg_id_hash,
              const char *sdp,
              size_t sdp_len);

/* Parse an inbound SAP packet. On success returns 0 and writes pointers
 * into the original buffer for the SDP section (`*sdp_out`, `*sdp_len_out`).
 * Rejects non-v1 packets, non-IPv4 source, compressed or encrypted
 * variants, and packets whose MIME is not "application/sdp". */
int sap_parse(const uint8_t *buf, size_t len,
              enum sap_kind *kind_out,
              uint32_t *origin_ipv4_be_out,
              uint16_t *msg_id_hash_out,
              const char **sdp_out,
              size_t *sdp_len_out);

/* Open an AF_INET UDP socket bound to egress via `iface_name` (may be
 * NULL), with IP_MULTICAST_TTL=32 and IP_MULTICAST_LOOP disabled. Returns
 * the fd on success, -1 on failure. The caller supplies the destination
 * sockaddr when calling sendto(). */
int sap_open_tx_socket(const char *iface_name);

/* Open an AF_INET UDP socket joined to SAP_IPV4_ADDR_STR:SAP_PORT on
 * `iface_name`, ready to recvfrom() announcements. Returns fd or -1. */
int sap_open_rx_socket(const char *iface_name);
