#pragma once

#include <stddef.h>

struct audio_source {
    int (*read)(struct audio_source *src, void *buf, size_t frames);
    void (*close)(struct audio_source *src);
    int channels;
    int rate;
    int bytes_per_sample;
    void *opaque;
};

struct audio_source *audio_source_test_open(int channels, int rate, int bytes_per_sample);
struct audio_source *audio_source_wav_open(const char *path);
struct audio_source *audio_source_alsa_open(const char *pcm_name, int channels, int rate);

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
