#include "audio_source.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#define TEST_FREQ_HZ  1000.0
#define TEST_AMP      0.5   /* -6 dBFS */

struct test_state {
    double phase;
    double step;
    int channels;
    int bytes_per_sample;
};

static void put_s24le_3(uint8_t *p, int32_t v)
{
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
}

static int test_read(struct audio_source *src, void *buf, size_t frames)
{
    struct test_state *st = src->opaque;
    uint8_t *p = buf;
    for (size_t i = 0; i < frames; i++) {
        double s = sin(st->phase) * TEST_AMP;
        st->phase += st->step;
        if (st->phase >= 2.0 * M_PI) {
            st->phase -= 2.0 * M_PI;
        }
        int32_t v = (int32_t)(s * 8388607.0); /* (2^23)-1 */
        for (int c = 0; c < st->channels; c++) {
            put_s24le_3(p, v);
            p += st->bytes_per_sample;
        }
    }
    return 0;
}

static void test_close(struct audio_source *src)
{
    free(src->opaque);
    free(src);
}

struct audio_source *audio_source_test_open(int channels, int rate, int bytes_per_sample)
{
    if (bytes_per_sample != 3) {
        return NULL; /* M1: s24le-3 only */
    }
    struct audio_source *src = calloc(1, sizeof(*src));
    struct test_state *st = calloc(1, sizeof(*st));
    if (!src || !st) {
        free(src);
        free(st);
        return NULL;
    }
    st->phase = 0.0;
    st->step = 2.0 * M_PI * TEST_FREQ_HZ / (double)rate;
    st->channels = channels;
    st->bytes_per_sample = bytes_per_sample;
    src->read = test_read;
    src->close = test_close;
    src->channels = channels;
    src->rate = rate;
    src->bytes_per_sample = bytes_per_sample;
    src->opaque = st;
    return src;
}
