/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dop.h"

uint32_t dop_carrier_rate_for_dsd(uint32_t dsd_byte_rate)
{
    /* Carrier = DSD bit rate / 16 = DSD byte rate / 2. */
    switch (dsd_byte_rate) {
    case  352800u: return  176400u; /* DSD64  */
    case  705600u: return  352800u; /* DSD128 */
    case 1411200u: return  705600u; /* DSD256 — Merging cap */
    case 2822400u: return 1411200u; /* DSD512 — out of AES67/Ravenna spec */
    default:       return 0u;
    }
}

uint32_t dop_dsd_byte_rate_for_carrier(uint32_t carrier_hz)
{
    switch (carrier_hz) {
    case  176400u: return  352800u; /* DSD64  */
    case  352800u: return  705600u; /* DSD128 */
    case  705600u: return 1411200u; /* DSD256 */
    case 1411200u: return 2822400u; /* DSD512 */
    default:       return 0u;
    }
}

void dop_encode(const uint8_t *dsd_bytes,
                uint8_t       *l24_out,
                int            channels,
                size_t         n_frames,
                struct dop_enc_state *st)
{
    uint8_t parity = st->marker_parity;
    for (size_t f = 0; f < n_frames; f++) {
        const uint8_t marker = parity ? DOP_MARKER_ODD : DOP_MARKER_EVEN;
        for (int c = 0; c < channels; c++) {
            /* Two DSD bytes per channel per L24 frame, sourced from
             * AOE-wire layout (byte_i * channels + c). The earlier
             * byte (older in time) goes to L24 byte 1 (just below the
             * marker); the later byte goes to L24 byte 2 (LSB). */
            const size_t dsd_idx_a = ((2u * f) + 0u) * (size_t)channels + (size_t)c;
            const size_t dsd_idx_b = ((2u * f) + 1u) * (size_t)channels + (size_t)c;
            const size_t l24_idx = (f * (size_t)channels + (size_t)c) * 3u;
            l24_out[l24_idx + 0] = marker;
            l24_out[l24_idx + 1] = dsd_bytes[dsd_idx_a];
            l24_out[l24_idx + 2] = dsd_bytes[dsd_idx_b];
        }
        parity ^= 1u;
    }
    st->marker_parity = parity;
}

size_t dop_decode(const uint8_t *l24_in,
                  uint8_t       *dsd_bytes_out,
                  int            channels,
                  size_t         n_frames,
                  struct dop_dec_state *st)
{
    uint8_t expected = st->marker_parity;
    size_t  good = 0;
    for (size_t f = 0; f < n_frames; f++) {
        const uint8_t want = expected ? DOP_MARKER_ODD : DOP_MARKER_EVEN;
        const uint8_t alt  = expected ? DOP_MARKER_EVEN : DOP_MARKER_ODD;
        /* Use channel 0's marker as the canonical one for this frame.
         * The spec requires all channels of a frame to carry the same
         * marker, but we don't fail the frame on a mismatch because
         * some bridges silently corrupt one channel — better to keep
         * decoding and let the audio side surface a defect than to drop
         * the frame entirely. */
        const uint8_t got = l24_in[(f * (size_t)channels + 0u) * 3u];
        int frame_ok = 0;
        if (got == want) {
            frame_ok = 1;
        } else if (got == alt) {
            /* One-frame slip — accept and resync parity. */
            expected ^= 1u;
            frame_ok = 1;
        }
        if (frame_ok) {
            good++;
            if (!st->locked) st->locked = 1;
        }
        for (int c = 0; c < channels; c++) {
            const size_t l24_idx = (f * (size_t)channels + (size_t)c) * 3u;
            const size_t dsd_idx_a = ((2u * f) + 0u) * (size_t)channels + (size_t)c;
            const size_t dsd_idx_b = ((2u * f) + 1u) * (size_t)channels + (size_t)c;
            dsd_bytes_out[dsd_idx_a] = l24_in[l24_idx + 1];
            dsd_bytes_out[dsd_idx_b] = l24_in[l24_idx + 2];
        }
        expected ^= 1u;
    }
    st->marker_parity = expected;
    return good;
}

int dop_detect(const uint8_t *l24_in, int channels, size_t n_frames)
{
    if (n_frames < 2 || channels < 1) return 0;
    /* Pick a starting parity from frame 0's MSB; demand frame 1 carries
     * the opposite marker; demand all subsequent frames continue the
     * alternation. Demand all channels of every frame share the marker. */
    const uint8_t m0 = l24_in[0];
    if (m0 != DOP_MARKER_EVEN && m0 != DOP_MARKER_ODD) return 0;
    const uint8_t m1 = l24_in[(size_t)channels * 3u];
    if (m1 == m0) return 0;
    if (m1 != DOP_MARKER_EVEN && m1 != DOP_MARKER_ODD) return 0;
    uint8_t expected = m0;
    for (size_t f = 0; f < n_frames; f++) {
        for (int c = 0; c < channels; c++) {
            const size_t idx = (f * (size_t)channels + (size_t)c) * 3u;
            if (l24_in[idx] != expected) return 0;
        }
        expected = (expected == DOP_MARKER_EVEN) ? DOP_MARKER_ODD
                                                 : DOP_MARKER_EVEN;
    }
    return 1;
}
