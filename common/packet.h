#pragma once

#include <stdint.h>

#define AOE_ETHERTYPE            0x88B5
#define AOE_MAGIC                0xA0
#define AOE_VERSION              0x01
#define AOE_HDR_LEN              16

#define AOE_FMT_RESERVED         0x00
#define AOE_FMT_PCM_S16LE        0x10
#define AOE_FMT_PCM_S24LE_3      0x11
#define AOE_FMT_PCM_S24LE_4      0x12
#define AOE_FMT_PCM_S32LE        0x13

#define AOE_FLAG_LAST_IN_GROUP   0x01
#define AOE_FLAG_DISCONTINUITY   0x02
#define AOE_FLAG_MARKER          0x04

struct aoe_hdr {
    uint8_t  magic;
    uint8_t  version;
    uint16_t stream_id;          /* big-endian on wire */
    uint32_t sequence;           /* big-endian on wire */
    uint32_t presentation_time;  /* big-endian on wire */
    uint8_t  channel_count;
    uint8_t  format;
    uint8_t  payload_count;
    uint8_t  flags;
} __attribute__((packed));

_Static_assert(sizeof(struct aoe_hdr) == AOE_HDR_LEN,
               "AoE header must be exactly 16 bytes");

void aoe_hdr_build(struct aoe_hdr *h,
                   uint16_t stream_id,
                   uint32_t sequence,
                   uint32_t presentation_time,
                   uint8_t channel_count,
                   uint8_t format,
                   uint8_t payload_count,
                   uint8_t flags);

int aoe_hdr_valid(const struct aoe_hdr *h);

/* -------- Control frames (Mode C feedback; see wire-format.md) -------- */

#define AOE_C_ETHERTYPE          0x88B6
#define AOE_C_MAGIC              0xA1
#define AOE_C_HDR_LEN            16

#define AOE_C_TYPE_FEEDBACK      0x01

struct aoe_c_hdr {
    uint8_t  magic;        /* 0xA1 */
    uint8_t  version;      /* 0x01 */
    uint8_t  frame_type;
    uint8_t  flags;
    uint16_t stream_id;    /* big-endian on wire */
    uint16_t sequence;     /* big-endian on wire */
    uint32_t value;        /* big-endian on wire; Q16.16 samples/ms for FEEDBACK */
    uint32_t reserved;     /* must be 0 */
} __attribute__((packed));

_Static_assert(sizeof(struct aoe_c_hdr) == AOE_C_HDR_LEN,
               "AoE-C header must be exactly 16 bytes");

/* Q16.16 samples-per-1-ms encoding. Matches USB UAC2 HS feedback format. */
#define AOE_FB_Q_FROM_KHZ(khz)   ((uint32_t)((khz) * 65536.0 + 0.5))
#define AOE_FB_Q_48K             0x00300000u   /* 48.000 samples/ms */

void aoe_c_hdr_build_feedback(struct aoe_c_hdr *h,
                              uint16_t stream_id,
                              uint16_t sequence,
                              uint32_t q16_16_samples_per_ms);

int aoe_c_hdr_valid(const struct aoe_c_hdr *h);
