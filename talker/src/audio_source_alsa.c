/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio_source.h"

#include <alsa/asoundlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ALSA capture with hold-last-sample fill on upstream underrun.
 *
 * The talker's emission rate is driven by Mode C feedback from the receiver,
 * which tracks the DAC's actual consumption rate via extended UAC2 async
 * feedback. When the upstream source (gmrender / MPD / DAW / …) writes at
 * nominal rate but the DAC pulls at nominal + ppm, the ring between them
 * (this ALSA capture device, typically snd-aloop) drains at the drift rate.
 * Once the ring reaches floor, each over-average packet arrives to find
 * one fewer sample than it wanted.
 *
 * We resolve that tail with a held-last-sample fill: the final valid frame
 * is repeated into the gap. At typical 20 ppm DAC drift this is ~1 sample
 * per second of steady-state duty (after the initial buffer drain),
 * producing ~20 µs plateaus with no transient — inaudible on program
 * material.
 *
 * This is NOT sample-rate conversion in the DSP sense; it is nearest-
 * neighbour tail-repeat, invoked at the point where the upstream source
 * cannot physically produce samples faster than its nominal clock allows.
 * See design.md §"Clock architecture" for the full framing.
 *
 * Buffer depth (CAPTURE_BUFFER_US set by the caller via --capture-buffer-ms)
 * bounds the period between fills during transient drift events — e.g.,
 * after a track change that refills gmrender's pipeline. */

struct alsa_state {
    snd_pcm_t *pcm;
    int channels;
    int bytes_per_sample;
    /* last_sample holds the most-recently-read channel frame, used to
     * hold-fill any tail the reader can't get from the ring. */
    uint8_t *last_sample;
    int have_last_sample;
    /* Stats, surfaced via get_stats. */
    uint64_t held_frames;
    uint64_t held_events;
};

static int alsa_read(struct audio_source *src, void *buf, size_t frames)
{
    struct alsa_state *st = src->opaque;
    uint8_t *p = buf;
    size_t remaining = frames;
    const size_t sample_bytes =
        (size_t)st->channels * (size_t)st->bytes_per_sample;

    /* Non-blocking reads, no wait: the emission loop calls us on a timerfd
     * tick and cannot tolerate being stalled by upstream producer jitter.
     * We drain whatever the ring holds right now and hold-fill any tail the
     * reader can't get — the next call will pick up whatever the producer
     * has written since. */
    while (remaining > 0) {
        snd_pcm_sframes_t avail = snd_pcm_avail(st->pcm);
        if (avail == -EPIPE || avail == -ESTRPIPE) {
            /* Capture xrun (rare on snd-aloop; happens on real capture
             * hardware). Recover and treat whatever we already collected
             * as the real data; hold-fill the rest. */
            int rr = snd_pcm_recover(st->pcm, (int)avail, 1);
            if (rr < 0) {
                fprintf(stderr,
                        "alsa capture: recover(avail): %s\n",
                        snd_strerror(rr));
                return -1;
            }
            break;
        }
        if (avail < 0) {
            int rr = snd_pcm_recover(st->pcm, (int)avail, 1);
            if (rr < 0) {
                fprintf(stderr,
                        "alsa capture: avail: %s\n",
                        snd_strerror((int)avail));
                return -1;
            }
            break;
        }

        if (avail == 0) {
            /* Ring empty right now. Do NOT wait — the emission loop is on a
             * timerfd and cannot be stalled by upstream producer jitter. Fall
             * through to hold-fill; the next call will pick up whatever the
             * producer has written since. */
            break;
        }

        const size_t to_read = (size_t)avail < remaining
                                   ? (size_t)avail : remaining;
        snd_pcm_sframes_t r = snd_pcm_readi(st->pcm, p, to_read);
        if (r == -EPIPE || r == -ESTRPIPE) {
            int rr = snd_pcm_recover(st->pcm, (int)r, 1);
            if (rr < 0) {
                fprintf(stderr,
                        "alsa capture: recover(read): %s\n",
                        snd_strerror(rr));
                return -1;
            }
            break;
        }
        if (r == -EAGAIN) {
            /* Non-blocking says "try again" — loop and recheck avail. */
            continue;
        }
        if (r < 0) {
            int rr = snd_pcm_recover(st->pcm, (int)r, 1);
            if (rr < 0) {
                fprintf(stderr,
                        "alsa capture: readi: %s\n",
                        snd_strerror((int)r));
                return -1;
            }
            continue;
        }
        /* r > 0: capture the last full frame for potential hold-fill. */
        memcpy(st->last_sample,
               p + ((size_t)r - 1) * sample_bytes,
               sample_bytes);
        st->have_last_sample = 1;
        remaining -= (size_t)r;
        p += (size_t)r * sample_bytes;
    }

    if (remaining > 0) {
        /* Hold-fill the tail. At typical drift rates these are single-sample
         * plateaus, imperceptible on program material. Silence-fill (zero) is
         * deliberately NOT used — a hard zero creates an audible discontinuity;
         * a held sample produces no transient. */
        if (st->have_last_sample) {
            for (size_t i = 0; i < remaining; i++) {
                memcpy(p + i * sample_bytes,
                       st->last_sample,
                       sample_bytes);
            }
        } else {
            /* Very early in playback, before any real sample has arrived.
             * Fill silence; the brief initial click is inherent to starting
             * a stream and is not drift-related. */
            memset(p, 0, remaining * sample_bytes);
        }
        st->held_frames += (uint64_t)remaining;
        st->held_events++;
    }

    return 0;
}

static void alsa_get_stats(struct audio_source *src,
                           uint64_t *held_frames_out,
                           uint64_t *held_events_out)
{
    struct alsa_state *st = src->opaque;
    if (held_frames_out) *held_frames_out = st->held_frames;
    if (held_events_out) *held_events_out = st->held_events;
}

static void alsa_close(struct audio_source *src)
{
    struct alsa_state *st = src->opaque;
    if (st) {
        if (st->pcm) {
            snd_pcm_drop(st->pcm);
            snd_pcm_close(st->pcm);
        }
        free(st->last_sample);
        free(st);
    }
    free(src);
}

struct audio_source *audio_source_alsa_open(const char *pcm_name, int channels,
                                            int rate, int buffer_us)
{
    if (buffer_us <= 0) buffer_us = 100000;   /* 100 ms default */

    snd_pcm_t *pcm = NULL;
    int err = snd_pcm_open(&pcm, pcm_name, SND_PCM_STREAM_CAPTURE,
                           SND_PCM_NONBLOCK);
    if (err < 0) {
        fprintf(stderr, "alsa capture: open(%s): %s\n",
                pcm_name, snd_strerror(err));
        return NULL;
    }

    err = snd_pcm_set_params(pcm,
                             SND_PCM_FORMAT_S24_3LE,
                             SND_PCM_ACCESS_RW_INTERLEAVED,
                             (unsigned int)channels,
                             (unsigned int)rate,
                             0,                    /* disable soft-resample */
                             (unsigned int)buffer_us);
    if (err < 0) {
        fprintf(stderr,
                "alsa capture: set_params on %s (S24_3LE ch=%d rate=%d): %s\n"
                "  (AOEther locks this format; configure your source daemon to match.)\n",
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

    err = snd_pcm_start(pcm);
    if (err < 0) {
        fprintf(stderr, "alsa capture: start: %s\n", snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }

    struct audio_source *src = calloc(1, sizeof(*src));
    struct alsa_state *st = calloc(1, sizeof(*st));
    uint8_t *last = calloc(1, (size_t)channels * 3);
    if (!src || !st || !last) {
        free(src);
        free(st);
        free(last);
        snd_pcm_close(pcm);
        return NULL;
    }
    st->pcm = pcm;
    st->channels = channels;
    st->bytes_per_sample = 3;
    st->last_sample = last;
    st->have_last_sample = 0;
    st->held_frames = 0;
    st->held_events = 0;

    src->read = alsa_read;
    src->close = alsa_close;
    src->get_stats = alsa_get_stats;
    src->channels = channels;
    src->rate = rate;
    src->bytes_per_sample = 3;
    src->opaque = st;

    /* Report the actually-granted buffer depth — ALSA / the kernel may
     * round to a device-supported period / buffer boundary. */
    snd_pcm_uframes_t got_buf = 0, got_period = 0;
    snd_pcm_get_params(pcm, &got_buf, &got_period);
    fprintf(stderr,
            "alsa capture: %s opened (S24_3LE ch=%d rate=%d "
            "buffer=%u frames (%u us requested, %u us granted) "
            "period=%u frames)\n",
            pcm_name, channels, rate,
            (unsigned)got_buf,
            (unsigned)buffer_us,
            rate > 0 ? (unsigned)((uint64_t)got_buf * 1000000ULL / (uint64_t)rate) : 0u,
            (unsigned)got_period);
    return src;
}
