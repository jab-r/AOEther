#include "audio_source.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* DSF (Sony DSD Stream File, spec rev 1.01) reader for the talker.
 *
 * Two format adaptations are required to feed the AOE wire format:
 *
 *   1. Channel interleave. DSF stores each channel's stream as a 4096-byte
 *      block, with one block per channel concatenated into a "frame":
 *          [ch0 block 4096B] [ch1 block 4096B] ... [chN block]
 *          [ch0 block 4096B] [ch1 block 4096B] ... [chN block]
 *          ...
 *      AOE wire wants byte-granular interleave:
 *          ch0[0] ch1[0] ... chN[0]  ch0[1] ch1[1] ... chN[1]  ...
 *
 *   2. Bit order. DSF with bits-per-sample=1 stores LSB-first within each
 *      byte; AOE wire is MSB-first (matching SND_PCM_FORMAT_DSD_U8 and the
 *      dsdsilence source). Each byte is bit-reversed on read.
 *      DSF files with bits-per-sample=8 are already MSB-first and pass
 *      through untouched — rare in practice but the spec permits it.
 *
 * Rate validation is left to the caller: the source reports its DSD byte
 * rate per channel in `src->rate`, and talker.c rejects mismatches against
 * the configured --format exactly the way it does for WAV. */

#define DSF_BLOCK_PER_CH       4096u

struct dsf_state {
    const uint8_t *map;
    size_t file_len;
    size_t data_off;        /* offset of first DSD byte in the file */
    size_t bytes_per_ch;    /* authoritative sample count, rounded to bytes */
    size_t pos_bc;          /* current byte offset within each channel */
    int    channels;
    int    reverse_bits;    /* 1 when DSF stored LSB-first (bits_per_sample=1) */
};

static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t rd_le64(const uint8_t *p)
{
    return (uint64_t)rd_le32(p) | ((uint64_t)rd_le32(p + 4) << 32);
}

static uint8_t reverse8(uint8_t b)
{
    b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

static int dsf_read(struct audio_source *src, void *buf, size_t bytes_per_ch)
{
    struct dsf_state *st = src->opaque;
    uint8_t *out = buf;
    const size_t ch    = (size_t)st->channels;
    const size_t block = DSF_BLOCK_PER_CH;
    const size_t total = st->bytes_per_ch;

    for (size_t i = 0; i < bytes_per_ch; i++) {
        if (st->pos_bc >= total) {
            st->pos_bc = 0;   /* loop like the WAV source */
        }
        const size_t block_idx = st->pos_bc / block;
        const size_t in_block  = st->pos_bc % block;
        const size_t frame_off = block_idx * ch * block;
        for (size_t c = 0; c < ch; c++) {
            const size_t off = st->data_off + frame_off + c * block + in_block;
            uint8_t b = st->map[off];
            if (st->reverse_bits) b = reverse8(b);
            out[i * ch + c] = b;
        }
        st->pos_bc++;
    }
    return 0;
}

static void dsf_close(struct audio_source *src)
{
    struct dsf_state *st = src->opaque;
    munmap((void *)st->map, st->file_len);
    free(st);
    free(src);
}

struct audio_source *audio_source_dsf_open(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("dsf: open");
        return NULL;
    }
    struct stat sb;
    if (fstat(fd, &sb) < 0) {
        perror("dsf: fstat");
        close(fd);
        return NULL;
    }
    const uint8_t *map = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        perror("dsf: mmap");
        return NULL;
    }

    /* DSD chunk (28 B) + fmt chunk (52 B) + data header (12 B) = 92 B min. */
    if (sb.st_size < 92 || memcmp(map, "DSD ", 4) != 0) {
        fprintf(stderr, "dsf: not a DSF file (missing 'DSD ' magic)\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    const uint64_t dsd_chunk_sz = rd_le64(map + 4);
    if (dsd_chunk_sz != 28) {
        fprintf(stderr, "dsf: unexpected DSD chunk size %llu (expected 28)\n",
                (unsigned long long)dsd_chunk_sz);
        munmap((void *)map, sb.st_size);
        return NULL;
    }

    const uint8_t *fmt = map + 28;
    if (memcmp(fmt, "fmt ", 4) != 0) {
        fprintf(stderr, "dsf: missing 'fmt ' chunk after DSD header\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    const uint64_t fmt_sz = rd_le64(fmt + 4);
    if (fmt_sz != 52) {
        fprintf(stderr, "dsf: unexpected fmt chunk size %llu (expected 52)\n",
                (unsigned long long)fmt_sz);
        munmap((void *)map, sb.st_size);
        return NULL;
    }

    const uint32_t version        = rd_le32(fmt + 12);
    const uint32_t format_id      = rd_le32(fmt + 16);
    const uint32_t channels       = rd_le32(fmt + 24);
    const uint32_t sampling_freq  = rd_le32(fmt + 28);
    const uint32_t bits_per_samp  = rd_le32(fmt + 32);
    const uint64_t sample_count   = rd_le64(fmt + 36);
    const uint32_t block_per_ch   = rd_le32(fmt + 44);

    if (version != 1 || format_id != 0) {
        fprintf(stderr, "dsf: unsupported DSF version=%u format_id=%u (need 1/0 = raw DSD)\n",
                version, format_id);
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    if (channels < 1 || channels > 8) {
        fprintf(stderr, "dsf: channel count %u out of range (1..8)\n", channels);
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    if (bits_per_samp != 1 && bits_per_samp != 8) {
        fprintf(stderr, "dsf: bits_per_sample=%u (spec allows only 1 or 8)\n", bits_per_samp);
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    if (block_per_ch != DSF_BLOCK_PER_CH) {
        fprintf(stderr, "dsf: block_size_per_channel=%u (spec always 4096)\n", block_per_ch);
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    /* AOEther carries DSD64/128/256/512/1024/2048 on the wire; DSD512+ is
     * delivered as split fragments per microframe (see wire-format.md
     * §"Cadence and fragmentation"). */
    if (sampling_freq != 2822400  && sampling_freq != 5644800 &&
        sampling_freq != 11289600 && sampling_freq != 22579200 &&
        sampling_freq != 45158400 && sampling_freq != 90316800) {
        fprintf(stderr,
                "dsf: sampling frequency %u Hz not supported "
                "(AOEther accepts DSD64/128/256/512/1024/2048 = "
                "2822400/5644800/11289600/22579200/45158400/90316800 Hz)\n",
                sampling_freq);
        munmap((void *)map, sb.st_size);
        return NULL;
    }

    /* data chunk */
    const uint8_t *dat = fmt + 52;
    if (dat + 12 > map + sb.st_size || memcmp(dat, "data", 4) != 0) {
        fprintf(stderr, "dsf: missing 'data' chunk\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    const uint64_t data_chunk_sz = rd_le64(dat + 4);
    const size_t   data_off      = (size_t)((dat + 12) - map);
    /* data_chunk_sz includes the 12-byte header; actual payload size is sz-12. */
    if (data_chunk_sz < 12 || data_off + (data_chunk_sz - 12) > (size_t)sb.st_size) {
        fprintf(stderr, "dsf: truncated data chunk\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }

    /* DSD samples are 1 bit each regardless of the bits_per_sample field —
     * that field only describes bit order within a byte (LSB-first for 1,
     * MSB-first for 8). Every byte of DSF payload carries exactly 8 DSD
     * samples of one channel. */
    const size_t bytes_per_ch = (size_t)(sample_count / 8u);
    if (bytes_per_ch == 0) {
        fprintf(stderr, "dsf: zero-length stream\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }

    /* The last block may be padded up to 4096 bytes even if the stream
     * ends mid-block. Verify the file is large enough to hold every block
     * we might index — ceil(bytes_per_ch / 4096) blocks, each channels*4096. */
    const size_t num_blocks   = (bytes_per_ch + DSF_BLOCK_PER_CH - 1) / DSF_BLOCK_PER_CH;
    const size_t needed_bytes = num_blocks * (size_t)channels * DSF_BLOCK_PER_CH;
    if (data_off + needed_bytes > (size_t)sb.st_size) {
        fprintf(stderr, "dsf: data chunk shorter than declared sample count\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }

    struct audio_source *src = calloc(1, sizeof(*src));
    struct dsf_state    *st  = calloc(1, sizeof(*st));
    if (!src || !st) {
        free(src);
        free(st);
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    st->map          = map;
    st->file_len     = sb.st_size;
    st->data_off     = data_off;
    st->bytes_per_ch = bytes_per_ch;
    st->pos_bc       = 0;
    st->channels     = (int)channels;
    st->reverse_bits = (bits_per_samp == 1);

    src->read             = dsf_read;
    src->close            = dsf_close;
    src->channels         = (int)channels;
    src->rate             = (int)(sampling_freq / 8u);   /* DSD bytes/sec/channel */
    src->bytes_per_sample = 1;
    src->opaque           = st;
    return src;
}
