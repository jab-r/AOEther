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
 * native-DSD path. A real DSF/DFF file reader is deferred to M8 alongside
 * DSD1024/2048 because it ties into the same per-DAC-quirk matrix. */
struct audio_source *audio_source_dsd_silence_open(int channels, int dsd_byte_rate);
