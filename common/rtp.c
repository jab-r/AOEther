/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rtp.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <stdint.h>

void rtp_hdr_build(struct rtp_hdr *h,
                   uint8_t  payload_type,
                   uint16_t sequence,
                   uint32_t timestamp,
                   uint32_t ssrc)
{
    /* V=2 (bits 7-6), P=0, X=0, CC=0. M=0 (bits 7-1 are PT). */
    h->vpxcc     = (uint8_t)(RTP_VERSION << 6);
    h->mpt       = (uint8_t)(payload_type & 0x7F);
    h->sequence  = htons(sequence);
    h->timestamp = htonl(timestamp);
    h->ssrc      = htonl(ssrc);
}

int rtp_hdr_parse(const struct rtp_hdr *h,
                  uint8_t  *out_pt,
                  uint16_t *out_sequence,
                  uint32_t *out_timestamp,
                  uint32_t *out_ssrc)
{
    const uint8_t version = (uint8_t)(h->vpxcc >> 6);
    const uint8_t padding = (uint8_t)((h->vpxcc >> 5) & 1);
    const uint8_t extflag = (uint8_t)((h->vpxcc >> 4) & 1);
    const uint8_t cc      = (uint8_t)(h->vpxcc & 0x0F);
    if (version != RTP_VERSION) return -1;
    /* AOEther only accepts the plain-vanilla RTP header today — no CSRC
     * list, no header extension, no padding. AES67 talkers don't use
     * these in practice. */
    if (cc != 0 || extflag != 0 || padding != 0) return -1;
    if (out_pt)        *out_pt        = (uint8_t)(h->mpt & 0x7F);
    if (out_sequence)  *out_sequence  = ntohs(h->sequence);
    if (out_timestamp) *out_timestamp = ntohl(h->timestamp);
    if (out_ssrc)      *out_ssrc      = ntohl(h->ssrc);
    return 0;
}

void rtp_swap24_inplace(uint8_t *buf, size_t nsamples)
{
    /* L24 samples are 3 bytes each; swap byte[0] and byte[2] per sample. */
    for (size_t i = 0; i < nsamples; i++) {
        uint8_t *s = buf + i * 3u;
        const uint8_t tmp = s[0];
        s[0] = s[2];
        s[2] = tmp;
    }
}

void rtp_swap16_inplace(uint8_t *buf, size_t nsamples)
{
    for (size_t i = 0; i < nsamples; i++) {
        uint8_t *s = buf + i * 2u;
        const uint8_t tmp = s[0];
        s[0] = s[1];
        s[1] = tmp;
    }
}
