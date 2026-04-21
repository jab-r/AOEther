#include "audio_source.h"
#include "packet.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* M6 talker-side DSD source. Emits the standard DSD-idle pattern (0x69)
 * interleaved across channels. On a real DSD-capable DAC this plays as
 * silence, which is exactly what we want from a wire-path / format-code
 * smoke test — anything else (a 1 kHz tone, modulated content) requires
 * a real sigma-delta modulator or DSF file reader, both deferred. */

struct dsd_state {
    int channels;
    int dsd_byte_rate;   /* bits/sec/channel ÷ 8 (e.g., 352800 for DSD64) */
};

static int dsd_read(struct audio_source *src, void *buf, size_t bytes_per_ch)
{
    struct dsd_state *st = src->opaque;
    /* `bytes_per_ch` is the talker's payload_count for this packet; total
     * bytes = bytes_per_ch × channels (interleaved by channel, MSB-first
     * within each byte per the wire format). */
    memset(buf, AOE_DSD_IDLE_BYTE, bytes_per_ch * (size_t)st->channels);
    return 0;
}

static void dsd_close(struct audio_source *src)
{
    free(src->opaque);
    free(src);
}

struct audio_source *audio_source_dsd_silence_open(int channels, int dsd_byte_rate)
{
    if (channels < 1 || dsd_byte_rate < 1) return NULL;

    struct audio_source *src = calloc(1, sizeof(*src));
    struct dsd_state    *st  = calloc(1, sizeof(*st));
    if (!src || !st) {
        free(src); free(st);
        return NULL;
    }
    st->channels = channels;
    st->dsd_byte_rate = dsd_byte_rate;
    src->read = dsd_read;
    src->close = dsd_close;
    src->channels = channels;
    src->rate = dsd_byte_rate;       /* "samples per sec per ch" semantic;
                                      * here that's DSD bytes per sec per ch */
    src->bytes_per_sample = 1;       /* one DSD byte per "sample" */
    src->opaque = st;
    return src;
}
