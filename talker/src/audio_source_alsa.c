#include "audio_source.h"

#include <alsa/asoundlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* M1 capture parameters. A generous period size keeps snd-aloop back-pressure
 * smooth; a generous buffer keeps us tolerant of source-daemon jitter without
 * shifting drift handling onto us (the DAC clock already propagates through
 * snd-aloop's ring — see design.md §"Clock architecture"). */
#define CAPTURE_PERIOD_US   5000      /* 5 ms */
#define CAPTURE_BUFFER_US   100000    /* 100 ms total */

struct alsa_state {
    snd_pcm_t *pcm;
    int channels;
    int bytes_per_sample;
};

static int alsa_read(struct audio_source *src, void *buf, size_t frames)
{
    struct alsa_state *st = src->opaque;
    uint8_t *p = buf;
    size_t remaining = frames;
    while (remaining > 0) {
        snd_pcm_sframes_t r = snd_pcm_readi(st->pcm, p, remaining);
        if (r == -EPIPE || r == -ESTRPIPE) {
            int rr = snd_pcm_recover(st->pcm, (int)r, 1);
            if (rr < 0) {
                fprintf(stderr, "alsa capture: recover: %s\n", snd_strerror(rr));
                return -1;
            }
            continue;
        }
        if (r == -EAGAIN) {
            continue;
        }
        if (r < 0) {
            int rr = snd_pcm_recover(st->pcm, (int)r, 1);
            if (rr < 0) {
                fprintf(stderr, "alsa capture: readi: %s\n", snd_strerror((int)r));
                return -1;
            }
            continue;
        }
        remaining -= (size_t)r;
        p += (size_t)r * st->channels * st->bytes_per_sample;
    }
    return 0;
}

static void alsa_close(struct audio_source *src)
{
    struct alsa_state *st = src->opaque;
    if (st) {
        if (st->pcm) {
            snd_pcm_drop(st->pcm);
            snd_pcm_close(st->pcm);
        }
        free(st);
    }
    free(src);
}

struct audio_source *audio_source_alsa_open(const char *pcm_name, int channels, int rate)
{
    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, pcm_name, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "alsa capture: open(%s): %s\n", pcm_name, snd_strerror(err));
        return NULL;
    }

    err = snd_pcm_set_params(pcm,
                             SND_PCM_FORMAT_S24_3LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             (unsigned int)channels,
                             (unsigned int)rate,
                             0,                    /* disable soft-resample */
                             CAPTURE_BUFFER_US);
    if (err < 0) {
        fprintf(stderr,
                "alsa capture: set_params on %s (S24_3LE ch=%d rate=%d): %s\n"
                "  (AOEther M1 locks this format; configure your source daemon to match.)\n",
                pcm_name, channels, rate, snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }

    err = snd_pcm_prepare(pcm);
    if (err < 0) {
        fprintf(stderr, "alsa capture: prepare: %s\n", snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }

    struct audio_source *src = calloc(1, sizeof(*src));
    struct alsa_state *st = calloc(1, sizeof(*st));
    if (!src || !st) {
        free(src);
        free(st);
        snd_pcm_close(pcm);
        return NULL;
    }
    st->pcm = pcm;
    st->channels = channels;
    st->bytes_per_sample = 3;

    src->read = alsa_read;
    src->close = alsa_close;
    src->channels = channels;
    src->rate = rate;
    src->bytes_per_sample = 3;
    src->opaque = st;

    fprintf(stderr,
            "alsa capture: %s opened (S24_3LE ch=%d rate=%d buffer_us=%d)\n",
            pcm_name, channels, rate, CAPTURE_BUFFER_US);
    return src;
}
