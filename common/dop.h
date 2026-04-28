/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* DoP (DSD-over-PCM) v1.1 encoder / decoder, used by Mode 4 (RTP/AES67)
 * to carry native DSD inside an L24 PCM carrier — the standard Ravenna
 * mechanism for DSD transport. Spec:
 *
 *   https://dsd-guide.com/sites/default/files/white-papers/DoP_open_Standard_1v1.pdf
 *
 * One DoP frame = one L24 PCM sample per channel, 24 bits laid out as:
 *
 *   byte 0 (MSB): marker, 0xFA on even frames and 0x05 on odd frames
 *                 (or vice versa — listeners detect either polarity)
 *   byte 1     : 8 DSD bits, MSB-first, oldest-first within the byte
 *   byte 2     : 8 more DSD bits, MSB-first
 *
 * So each L24 frame at the carrier rate carries 16 DSD bits per channel.
 * The carrier rate is therefore (DSD bit rate / 16):
 *
 *   DSD64  ( 2.8224 MHz)  →  176_400 Hz L24 carrier
 *   DSD128 ( 5.6448 MHz)  →  352_800 Hz L24 carrier  (= DXD rate)
 *   DSD256 (11.2896 MHz)  →  705_600 Hz L24 carrier  (Merging cap)
 *   DSD512 (22.5792 MHz)  → 1_411_200 Hz L24 carrier (out of AES67/Ravenna
 *                                                     spec; supported for
 *                                                     non-Merging gear)
 *
 * AOEther's native-DSD source path produces DSD as MSB-first bytes,
 * channel-interleaved at byte granularity (matching SND_PCM_FORMAT_DSD_U8
 * and the AOE wire layout — index `byte_i * channels + c`). The encoder
 * and decoder use this same layout for their DSD byte I/O so they can
 * sit directly between the DSD source/sink path and the RTP wire path
 * without an extra transpose. Each DoP L24 frame consumes / produces
 * two consecutive DSD-byte rows (i.e. `byte_i = 2*f` and `byte_i = 2*f+1`)
 * for every channel.
 *
 * The marker alternates per L24 frame (i.e. per pair of DSD bytes), and
 * is the *same* marker across all channels of a given frame. The encoder
 * tracks parity in caller-supplied state so streaming across packet
 * boundaries works.
 *
 * On the wire AOEther emits L24 in network byte order (RTP/AES67 spec):
 * the marker byte must be sent first (most-significant byte of the L24
 * sample). The encoder writes its output in big-endian L24 directly so
 * callers can hand the buffer to the RTP egress path with no further
 * byte-swap. Decoder accepts BE input and writes native-DSD output. */

/* DoP marker bytes. The spec calls these "FA" and "05"; emitting either
 * polarity is acceptable to listeners — they look for the alternation,
 * not a starting phase. AOEther emits FA on even frames, 05 on odd. */
#define DOP_MARKER_EVEN    0xFAu
#define DOP_MARKER_ODD     0x05u

/* L24 carrier rate (Hz) for a given DSD byte rate (DSD bit rate / 8).
 * Returns 0 if `dsd_byte_rate` is not a recognized DSD rate. */
uint32_t dop_carrier_rate_for_dsd(uint32_t dsd_byte_rate);

/* Inverse of dop_carrier_rate_for_dsd: returns the DSD byte rate that
 * corresponds to a DoP-carrying L24 rate, or 0 if `carrier_hz` isn't a
 * standard DoP carrier. Used by the receiver to detect-and-classify an
 * incoming L24 stream when --unwrap-dop is set. */
uint32_t dop_dsd_byte_rate_for_carrier(uint32_t carrier_hz);

/* Encoder state. Caller zero-initializes once per stream and passes the
 * same pointer on every call so marker parity is maintained across packet
 * boundaries. Not thread-safe; one encoder per stream. */
struct dop_enc_state {
    uint8_t  marker_parity;  /* 0 = next frame uses MARKER_EVEN, 1 = ODD */
};

/* Encode `n_frames` L24 carrier frames from interleaved DSD bytes.
 *
 *   dsd_bytes  : input, AOE-wire layout — `byte_i * channels + c`,
 *                MSB-first within each byte.
 *                Length = n_frames * 2 * channels (each L24 frame eats
 *                two DSD-byte rows × all channels).
 *   l24_out    : output, big-endian L24 (24-bit big-endian PCM samples),
 *                channels-interleaved. Length = n_frames * 3 * channels.
 *   channels   : channel count (>= 1).
 *   n_frames   : number of L24 carrier frames to produce.
 *   st         : encoder state, advanced by n_frames mod 2.
 */
void dop_encode(const uint8_t *dsd_bytes,
                uint8_t       *l24_out,
                int            channels,
                size_t         n_frames,
                struct dop_enc_state *st);

/* Decoder state. */
struct dop_dec_state {
    uint8_t  marker_parity;  /* expected parity for next frame */
    uint8_t  locked;         /* 1 once we've seen a valid alternation */
};

/* Decode `n_frames` of big-endian L24 back to channels-interleaved DSD
 * bytes (length n_frames * 2 * channels). Returns the number of frames
 * whose markers passed the alternation check. A return of 0 means the
 * input is not DoP (the receiver should treat it as plain L24 PCM).
 *
 * The decoder is permissive: a single bad marker doesn't abort, it just
 * doesn't increment the good-frames count. The caller decides what to
 * do (e.g. fall back to PCM passthrough after N bad frames in a row). */
size_t dop_decode(const uint8_t *l24_in,
                  uint8_t       *dsd_bytes_out,
                  int            channels,
                  size_t         n_frames,
                  struct dop_dec_state *st);

/* Detect whether a buffer of big-endian L24 samples is DoP without
 * mutating any state. Returns 1 if the marker pattern (alternating
 * 0xFA / 0x05 in the MSB across consecutive frames, same value across
 * channels within a frame) is present for the full buffer; 0 otherwise.
 * Useful at receiver startup to choose between passthrough vs unwrap. */
int dop_detect(const uint8_t *l24_in, int channels, size_t n_frames);
