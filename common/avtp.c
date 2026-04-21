#include "avtp.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <string.h>

/* Bit positions within the 32-bit format_specific word, mirroring libavtp:
 *   FORMAT             shift 24, width 8
 *   NSR                shift 20, width 4
 *   (reserved)         shift 18, width 2
 *   CHANNELS_PER_FRAME shift 8,  width 10  (bits 17..8 — leaks into octet 17 LSBs)
 *   BIT_DEPTH          shift 0,  width 8
 *
 * The bit layout above produces the exact byte pattern that Hive,
 * Wireshark's AVTP dissector, and Milan listeners expect. */

int avtp_aaf_nsr_from_hz(int rate_hz, uint8_t *nsr)
{
    switch (rate_hz) {
    case 8000:   *nsr = AAF_NSR_8000;   return 0;
    case 16000:  *nsr = AAF_NSR_16000;  return 0;
    case 24000:  *nsr = AAF_NSR_24000;  return 0;
    case 32000:  *nsr = AAF_NSR_32000;  return 0;
    case 44100:  *nsr = AAF_NSR_44100;  return 0;
    case 48000:  *nsr = AAF_NSR_48000;  return 0;
    case 88200:  *nsr = AAF_NSR_88200;  return 0;
    case 96000:  *nsr = AAF_NSR_96000;  return 0;
    case 176400: *nsr = AAF_NSR_176400; return 0;
    case 192000: *nsr = AAF_NSR_192000; return 0;
    default: return -1;
    }
}

int avtp_aaf_hz_from_nsr(uint8_t nsr, int *rate_hz)
{
    switch (nsr) {
    case AAF_NSR_8000:   *rate_hz = 8000;   return 0;
    case AAF_NSR_16000:  *rate_hz = 16000;  return 0;
    case AAF_NSR_24000:  *rate_hz = 24000;  return 0;
    case AAF_NSR_32000:  *rate_hz = 32000;  return 0;
    case AAF_NSR_44100:  *rate_hz = 44100;  return 0;
    case AAF_NSR_48000:  *rate_hz = 48000;  return 0;
    case AAF_NSR_88200:  *rate_hz = 88200;  return 0;
    case AAF_NSR_96000:  *rate_hz = 96000;  return 0;
    case AAF_NSR_176400: *rate_hz = 176400; return 0;
    case AAF_NSR_192000: *rate_hz = 192000; return 0;
    default: return -1;
    }
}

void avtp_aaf_hdr_build(struct avtp_aaf_hdr *h,
                        uint64_t stream_id,
                        uint8_t  seq_num,
                        uint32_t avtp_timestamp,
                        uint8_t  aaf_format,
                        uint8_t  nsr,
                        uint16_t channels_per_frame,
                        uint8_t  bit_depth,
                        uint16_t stream_data_length)
{
    /* Octets 0-3: subtype | sv | version | mr | rsv | tv | seq_num | rsv | tu */
    uint32_t sd = ((uint32_t)AVTP_SUBTYPE_AAF << 24)
                | ((uint32_t)1u           << 23)   /* sv = stream_id valid */
                /* version = 0  → bits 22..20 stay zero */
                /* mr = 0       → bit 19 zero */
                /* rsv          → bits 18..17 zero */
                | ((avtp_timestamp ? 1u : 0u) << 16)  /* tv */
                | ((uint32_t)seq_num      << 8);
                /* tu = 0 (timestamp not uncertain) */
    h->subtype_data = htonl(sd);

    /* stream_id is opaque from AOEther's perspective; pass through. */
    uint32_t sid_hi = (uint32_t)(stream_id >> 32);
    uint32_t sid_lo = (uint32_t)(stream_id & 0xffffffffu);
    uint32_t be_hi = htonl(sid_hi);
    uint32_t be_lo = htonl(sid_lo);
    memcpy((uint8_t *)&h->stream_id,     &be_hi, 4);
    memcpy((uint8_t *)&h->stream_id + 4, &be_lo, 4);

    h->avtp_timestamp = htonl(avtp_timestamp);

    uint32_t fs = ((uint32_t)aaf_format             << 24)
                | (((uint32_t)nsr & 0xfu)           << 20)
                | (((uint32_t)channels_per_frame & 0x3ffu) << 8)
                | ((uint32_t)bit_depth & 0xffu);
    h->format_specific = htonl(fs);

    /* SP = 0 (normal mode), EVT = 0, reserved = 0. */
    uint32_t pi = ((uint32_t)stream_data_length << 16);
    h->packet_info = htonl(pi);
}

int avtp_aaf_hdr_parse(const struct avtp_aaf_hdr *h,
                       uint64_t *stream_id,
                       uint8_t  *seq_num,
                       uint32_t *avtp_timestamp,
                       uint8_t  *aaf_format,
                       uint8_t  *nsr,
                       uint16_t *channels_per_frame,
                       uint8_t  *bit_depth,
                       uint16_t *stream_data_length)
{
    uint32_t sd = ntohl(h->subtype_data);
    uint8_t  subtype = (uint8_t)((sd >> 24) & 0xff);
    uint8_t  version = (uint8_t)((sd >> 20) & 0x07);
    if (subtype != AVTP_SUBTYPE_AAF) return -1;
    if (version != 0) return -1;

    if (seq_num) *seq_num = (uint8_t)((sd >> 8) & 0xff);

    if (stream_id) {
        uint32_t hi, lo;
        memcpy(&hi, (const uint8_t *)&h->stream_id, 4);
        memcpy(&lo, (const uint8_t *)&h->stream_id + 4, 4);
        *stream_id = ((uint64_t)ntohl(hi) << 32) | (uint64_t)ntohl(lo);
    }
    if (avtp_timestamp) *avtp_timestamp = ntohl(h->avtp_timestamp);

    uint32_t fs = ntohl(h->format_specific);
    if (aaf_format)         *aaf_format = (uint8_t)((fs >> 24) & 0xff);
    if (nsr)                *nsr        = (uint8_t)((fs >> 20) & 0x0f);
    if (channels_per_frame) *channels_per_frame = (uint16_t)((fs >> 8) & 0x3ff);
    if (bit_depth)          *bit_depth  = (uint8_t)(fs & 0xff);

    uint32_t pi = ntohl(h->packet_info);
    if (stream_data_length) *stream_data_length = (uint16_t)((pi >> 16) & 0xffff);

    return 0;
}

void avtp_swap24_inplace(void *buf, size_t n_samples)
{
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < n_samples; i++) {
        uint8_t b0 = p[0];
        p[0] = p[2];
        p[2] = b0;
        p += 3;
    }
}
