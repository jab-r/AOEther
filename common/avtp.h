/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

/* IEEE 1722-2016 AVTP transport for AOEther (Mode 2, Milan interop).
 *
 * For PCM streams we emit AVTP AAF (subtype 0x02) frames directly. The
 * AOE header is replaced by the 24-byte AVTP+AAF header on the wire,
 * but Mode C clock-discipline feedback continues to flow on EtherType
 * 0x88B6 unchanged — AVTP knows nothing about it and Milan listeners
 * simply ignore it. See docs/wire-format.md §"Mode 2".
 *
 * AAF samples on the wire are big-endian per IEEE 1722-2016, unlike
 * the AOE native wrapper which carries ALSA s24le directly. The talker
 * byte-swaps on egress and the receiver byte-swaps on ingress; ALSA on
 * both ends still sees s24le-3.
 */

#include <stddef.h>
#include <stdint.h>

#define AVTP_ETHERTYPE        0x22F0
#define AVTP_HDR_LEN          24      /* 4 (subtype_data) + 8 (stream_id) +
                                       * 4 (avtp_timestamp) + 4 (format_specific)
                                       * + 4 (packet_info) */

#define AVTP_SUBTYPE_AAF      0x02

/* AAF format codes (IEEE 1722-2016 Table 19). */
#define AAF_FORMAT_USER         0x00
#define AAF_FORMAT_FLOAT32      0x01
#define AAF_FORMAT_INT32        0x02
#define AAF_FORMAT_INT24        0x03
#define AAF_FORMAT_INT16        0x04
#define AAF_FORMAT_AES3_32      0x05

/* AAF nominal-sample-rate codes (IEEE 1722-2016 Table 20). */
#define AAF_NSR_RESERVED        0x00
#define AAF_NSR_8000            0x01
#define AAF_NSR_16000           0x02
#define AAF_NSR_32000           0x03
#define AAF_NSR_44100           0x04
#define AAF_NSR_48000           0x05
#define AAF_NSR_88200           0x06
#define AAF_NSR_96000           0x07
#define AAF_NSR_176400          0x08
#define AAF_NSR_192000          0x09
#define AAF_NSR_24000           0x0A

/* The packed AVTP-AAF header. All multi-byte fields are big-endian on the
 * wire; the four 32-bit words below are stored network-order. Helpers
 * avtp_aaf_hdr_build() / avtp_aaf_hdr_parse() handle the bit packing. */
struct avtp_aaf_hdr {
    uint32_t subtype_data;     /* subtype | sv | version | mr | tv | seq_num | tu */
    uint64_t stream_id;        /* opaque 64-bit stream ID */
    uint32_t avtp_timestamp;   /* gPTP nanoseconds, low 32 bits */
    uint32_t format_specific;  /* format | nsr | channels_per_frame | bit_depth */
    uint32_t packet_info;      /* stream_data_length (high 16) | flags (low 16) */
} __attribute__((packed));

_Static_assert(sizeof(struct avtp_aaf_hdr) == AVTP_HDR_LEN,
               "AVTP-AAF header must be exactly 24 bytes");

/* Map an audio sample rate in Hz to the AAF nsr code. Returns 0 on success,
 * -1 if the rate is not a standard AAF rate. */
int avtp_aaf_nsr_from_hz(int rate_hz, uint8_t *nsr);

/* Inverse of the above. Returns 0 / Hz on success, -1 on unknown nsr. */
int avtp_aaf_hz_from_nsr(uint8_t nsr, int *rate_hz);

/* Build an AVTP-AAF header in network byte order.
 *
 * - sv is implicitly 1, version 0, mr 0, tu 0 (typical Milan listener PCM).
 * - tv is 1 iff avtp_timestamp != 0 (no PTP on M5 yet → pass 0).
 * - sp is 0 (normal — every packet carries a presentation time when tv=1).
 * - evt is 0.
 * - bit_depth is 24 for INT24, 32 for INT32/FLOAT32, 16 for INT16.
 */
void avtp_aaf_hdr_build(struct avtp_aaf_hdr *h,
                        uint64_t stream_id,
                        uint8_t  seq_num,
                        uint32_t avtp_timestamp,
                        uint8_t  aaf_format,
                        uint8_t  nsr,
                        uint16_t channels_per_frame,
                        uint8_t  bit_depth,
                        uint16_t stream_data_length);

/* Parse an AVTP-AAF header. Returns 0 on success and fills the out params
 * with host-order values. Returns -1 if the subtype is not AAF or the AVTP
 * version is unknown. */
int avtp_aaf_hdr_parse(const struct avtp_aaf_hdr *h,
                       uint64_t *stream_id,
                       uint8_t  *seq_num,
                       uint32_t *avtp_timestamp,
                       uint8_t  *aaf_format,
                       uint8_t  *nsr,
                       uint16_t *channels_per_frame,
                       uint8_t  *bit_depth,
                       uint16_t *stream_data_length);

/* Byte-swap an interleaved buffer of packed 24-bit samples in place.
 * AVTP AAF carries 24-bit samples big-endian; AOEther's source/sink path
 * is ALSA s24le-3. Used on the AVTP egress / ingress edge only. */
void avtp_swap24_inplace(void *buf, size_t n_samples);
