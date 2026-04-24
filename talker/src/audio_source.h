/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

struct audio_source {
    int (*read)(struct audio_source *src, void *buf, size_t frames);
    void (*close)(struct audio_source *src);
    int channels;
    int rate;
    int bytes_per_sample;
    void *opaque;
    /* Optional: report capture-edge "held-last-sample" statistics. Implemented
     * by sources that can underrun against a DAC-driven consumption rate
     * (e.g. ALSA capture from snd-aloop) — NULL for file / test-tone / DSF
     * sources where there is no upstream-vs-consumer rate mismatch to
     * absorb. See design.md §"Clock architecture" for how this relates to
     * extending UAC2 async feedback across Ethernet. */
    void (*get_stats)(struct audio_source *src,
                      uint64_t *held_frames_out,
                      uint64_t *held_events_out);
};

struct audio_source *audio_source_test_open(int channels, int rate, int bytes_per_sample);
struct audio_source *audio_source_wav_open(const char *path);
/* ALSA capture with a caller-specified ring-buffer depth. `buffer_us` bounds
 * the minimum time between hold-last-sample fills under positive DAC drift:
 * at 20 ppm and 48 kHz, 100 ms buys ~85 minutes per fill, 200 ms buys ~170
 * minutes, 500 ms buys ~7 hours. Each fill is one held sample (~20 µs) which
 * is inaudible on program material — not a transient, just a brief plateau.
 *
 * `capture_format` selects the ALSA hardware format opened on the capture
 * device, matching whatever the upstream renderer (snd-aloop's other half)
 * is writing. Supported values:
 *   "pcm_s24_3le" — 3 bytes/channel, AOE PCM wire-format compatible.
 *   "dsd_u8"      — 1 byte/channel, AOE native-DSD wire-format compatible
 *                   (1:1 byte passthrough, no per-byte reordering).
 * DSD wider variants (dsd_u16_le, dsd_u16_be, dsd_u32_le, dsd_u32_be) are
 * not yet supported on capture — for the snd-aloop bridge pattern the
 * operator controls both sides of the loopback and can pin the renderer to
 * dsd_u8. */
struct audio_source *audio_source_alsa_open(const char *pcm_name,
                                            const char *capture_format,
                                            int channels,
                                            int rate, int buffer_us);

/* DSD silence source (M6). `dsd_byte_rate` is bits/sec/channel ÷ 8; for
 * DSD64 it's 352800, for DSD256 it's 1411200. `read()` returns the idle
 * pattern (`AOE_DSD_IDLE_BYTE`) interleaved across channels — acoustically
 * silent on a real DAC, but exercises the full wire-format and ALSA
 * native-DSD path. */
struct audio_source *audio_source_dsd_silence_open(int channels, int dsd_byte_rate);

/* DSF file reader. Opens a Sony DSF (DSD Stream File) and exposes its
 * contents through the standard audio_source interface. The reader
 * deinterleaves DSF's 4096-byte-per-channel blocks into AOE's byte-
 * granular wire interleave and bit-reverses each byte when the file is
 * stored LSB-first (the usual bits_per_sample=1 case). Supported rates:
 * DSD64 through DSD2048 (DSD512+ via per-microframe packet splitting on the
 * wire). The caller must verify the DSF's reported channels and rate match
 * --format / --channels, exactly as for the WAV source. */
struct audio_source *audio_source_dsf_open(const char *path);

/* DFF (Philips/Sony DSDIFF) file reader. DSDIFF's byte interleave already
 * matches AOE's wire format (byte-granular channel interleave, MSB-first
 * within each byte) so read() memcpy's straight from the mmap. Uncompressed
 * DSD only — DST-compressed DSDIFF files are rejected. Supported rates:
 * DSD64 through DSD2048. Like the DSF reader, the caller must verify the
 * file's reported channels and rate match --format / --channels. */
struct audio_source *audio_source_dff_open(const char *path);
