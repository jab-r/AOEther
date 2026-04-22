#pragma once

#include <stddef.h>
#include <stdint.h>

/* AOEther Mode 4 (M9) — RTP/AES67 wire encoding.
 *
 * Mode 4 carries PCM audio as RTP over UDP for interop with the Ravenna /
 * AES67 ecosystem (any AES67 device: aes67-linux-daemon, Merging Anubis,
 * Neumann MT 48, Dante-with-AES67-mode, Ravenna-native gear, etc.). The
 * header is standard RTP (RFC 3550) with an L24 or L16 payload per RFC
 * 3190 / AES67 §7.
 *
 * The RTP header below is plain IETF — there's no AOEther-specific
 * framing on top. The stream is identified by the RTP SSRC and the UDP
 * (destination address, port) tuple; SDP / SAP (M9 Phase B) carry the
 * session description out-of-band.
 *
 * Bytes-on-wire (big-endian throughout, unlike our LE AoE path):
 *
 *    0                   1                   2                   3
 *    0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |V=2|P|X|  CC   |M|     PT      |       sequence number         |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |                           timestamp                           |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *   |            synchronization source (SSRC) identifier           |
 *   +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * AOEther emits: V=2, P=0, X=0, CC=0, M=0. Payload type (PT) is dynamic
 * per AES67 — controllers normally negotiate it via SDP; the baseline
 * default 96 is used below until SDP lands. The RTP timestamp is the
 * stream's media clock sample count (advancing by `samples_per_packet`
 * per packet at the stream's sample rate).
 *
 * Payload (after the 12-byte header):
 *   - L24: signed 24-bit PCM, big-endian on the wire, channel-interleaved.
 *     `channels × samples_per_packet × 3` bytes.
 *   - L16: signed 16-bit PCM, big-endian on the wire, channel-interleaved.
 *     `channels × samples_per_packet × 2` bytes.
 *
 * AOEther's ALSA source/sink is little-endian (`S24_3LE` / `S16_LE`), so
 * the talker byte-swaps on egress and the receiver byte-swaps on ingress.
 * This mirrors the AAF (Mode 2) data-path endianness flip.
 *
 * Cadence (PTIME):
 *   - 1 ms is the AES67 standard default (48 samples/packet at 48 kHz).
 *   - 125 µs is the AES67 "low-latency" profile (6 samples/packet at
 *     48 kHz). Matches AOEther's existing 8000 pps microframe cadence.
 *   - Others (250 µs, 333 µs) exist in the spec but AOEther only supports
 *     the two above for M9 Phase A.
 *
 * Mode C clock-discipline feedback is NOT emitted in Mode 4. AES67
 * devices expect PTPv2 (default profile) as the time source, not our
 * UAC2-shape feedback. Running AOEther's Mode 4 path against a strict
 * AES67 endpoint requires a PTPv2 grandmaster on the network. See
 * docs/recipe-aes67.md for deployment notes. */

#define RTP_HDR_LEN             12
#define RTP_VERSION             2

/* Default dynamic payload type until SDP-negotiated values arrive. */
#define RTP_DEFAULT_PT_L24      96
#define RTP_DEFAULT_PT_L16      97

/* AES67 default destination port for RTP media. */
#define RTP_DEFAULT_PORT        5004

/* Supported PTIME values for M9 Phase A, in microseconds. */
#define RTP_PTIME_US_1MS        1000
#define RTP_PTIME_US_125US      125

struct rtp_hdr {
    uint8_t  vpxcc;         /* V=2, P, X, CC */
    uint8_t  mpt;           /* M, PT (payload type) */
    uint16_t sequence;      /* big-endian on wire */
    uint32_t timestamp;     /* big-endian on wire; media-clock sample count */
    uint32_t ssrc;          /* big-endian on wire */
} __attribute__((packed));

_Static_assert(sizeof(struct rtp_hdr) == RTP_HDR_LEN,
               "RTP header must be exactly 12 bytes");

/* Build an RTP header into `h`. V=2, P=0, X=0, CC=0, M=0 — all values
 * controllers normally expect from an AES67 talker. */
void rtp_hdr_build(struct rtp_hdr *h,
                   uint8_t  payload_type,
                   uint16_t sequence,
                   uint32_t timestamp,
                   uint32_t ssrc);

/* Parse + validate an RTP header. Returns 0 on success and fills the
 * out-parameters; returns -1 if the header is malformed (bad version,
 * unexpected extensions, too-small input).
 *
 * Only V=2 with CC=0 and X=0 is accepted — CSRC lists and header
 * extensions are rare on AES67 audio streams and AOEther doesn't need
 * them today. Extending to accept CC>0 or X=1 means parsing past the
 * fixed header to find the payload start. */
int rtp_hdr_parse(const struct rtp_hdr *h,
                  uint8_t  *out_pt,
                  uint16_t *out_sequence,
                  uint32_t *out_timestamp,
                  uint32_t *out_ssrc);

/* Byte-swap 24-bit samples in-place between BE (wire) and LE (ALSA).
 * `nsamples` is total samples (channels × frames). The operation is
 * its own inverse — call it on the same buffer to swap both ways. */
void rtp_swap24_inplace(uint8_t *buf, size_t nsamples);

/* Byte-swap 16-bit samples in-place. Same inverse property as above. */
void rtp_swap16_inplace(uint8_t *buf, size_t nsamples);
