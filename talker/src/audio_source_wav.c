#include "audio_source.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct wav_state {
    const uint8_t *map;
    size_t file_len;
    size_t data_off;
    size_t data_len;
    size_t pos;
    int channels;
    int bytes_per_sample;
};

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t rd_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int wav_read(struct audio_source *src, void *buf, size_t frames)
{
    struct wav_state *st = src->opaque;
    size_t frame_bytes = (size_t)st->channels * st->bytes_per_sample;
    size_t want = frames * frame_bytes;
    uint8_t *out = buf;
    while (want > 0) {
        size_t avail = st->data_len - st->pos;
        if (avail == 0) {
            st->pos = 0;
            continue;
        }
        size_t take = want < avail ? want : avail;
        memcpy(out, st->map + st->data_off + st->pos, take);
        st->pos += take;
        out += take;
        want -= take;
    }
    return 0;
}

static void wav_close(struct audio_source *src)
{
    struct wav_state *st = src->opaque;
    munmap((void *)st->map, st->file_len);
    free(st);
    free(src);
}

struct audio_source *audio_source_wav_open(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("wav: open");
        return NULL;
    }
    struct stat sb;
    if (fstat(fd, &sb) < 0) {
        perror("wav: fstat");
        close(fd);
        return NULL;
    }
    const uint8_t *map = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        perror("wav: mmap");
        return NULL;
    }

    if (sb.st_size < 44 ||
        memcmp(map, "RIFF", 4) != 0 ||
        memcmp(map + 8, "WAVE", 4) != 0) {
        fprintf(stderr, "wav: not a RIFF/WAVE file\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }

    size_t off = 12;
    int fmt_tag = 0, channels = 0, rate = 0, bits = 0;
    size_t data_off = 0, data_len = 0;
    while (off + 8 <= (size_t)sb.st_size) {
        const uint8_t *ck = map + off;
        uint32_t sz = rd_le32(ck + 4);
        if (memcmp(ck, "fmt ", 4) == 0 && sz >= 16) {
            fmt_tag  = rd_le16(ck + 8);
            channels = rd_le16(ck + 10);
            rate     = (int)rd_le32(ck + 12);
            bits     = rd_le16(ck + 22);
        } else if (memcmp(ck, "data", 4) == 0) {
            data_off = off + 8;
            data_len = sz;
            break;
        }
        off += 8 + sz + (sz & 1);
    }

    /* M2 accepts any channel count and any supported rate, but the sample
     * format is still locked to 24-bit little-endian (s24le-3 on the wire). */
    static const int rates[] = { 44100, 48000, 88200, 96000, 176400, 192000 };
    int rate_ok = 0;
    for (size_t i = 0; i < sizeof(rates)/sizeof(rates[0]); i++) {
        if (rate == rates[i]) { rate_ok = 1; break; }
    }
    if (fmt_tag != 1 || channels < 1 || channels > 64 || !rate_ok || bits != 24) {
        fprintf(stderr,
                "wav: need PCM 1..64ch 24-bit at a supported rate "
                "(44.1/48/88.2/96/176.4/192 kHz); got tag=%d ch=%d rate=%d bits=%d\n",
                fmt_tag, channels, rate, bits);
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    if (data_off == 0 || data_off + data_len > (size_t)sb.st_size) {
        fprintf(stderr, "wav: missing or truncated data chunk\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }

    struct audio_source *src = calloc(1, sizeof(*src));
    struct wav_state *st = calloc(1, sizeof(*st));
    if (!src || !st) {
        free(src);
        free(st);
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    st->map = map;
    st->file_len = sb.st_size;
    st->data_off = data_off;
    st->data_len = data_len;
    st->channels = channels;
    st->bytes_per_sample = 3;

    src->read = wav_read;
    src->close = wav_close;
    src->channels = channels;
    src->rate = rate;
    src->bytes_per_sample = 3;
    src->opaque = st;
    return src;
}
