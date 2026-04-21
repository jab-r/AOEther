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
