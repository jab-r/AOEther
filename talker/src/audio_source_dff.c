#include "audio_source.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* DFF (Philips/Sony DSDIFF 1.5) reader for the talker.
 *
 * DSDIFF's payload layout happens to match the AOE native-DSD wire format
 * exactly — samples are stored byte-interleaved across channels, MSB-first
 * within each byte. No bit-reverse, no block-deinterleave. `read()` just
 * memcpy's bytes straight from the mmap'd DSD data chunk.
 *
 * Multi-byte integer fields are big-endian (opposite of DSF's little-endian
 * convention). Sub-chunks are AIFF-style: 4-byte ID, 8-byte BE size, data
 * padded to an even byte count.
 *
 * Only uncompressed DSD ("DSD " compression type) is accepted; DST-compressed
 * files are rejected — decompression would belong in a DFF-specific layer
 * and we don't own it. */

struct dff_state {
    const uint8_t *map;
    size_t file_len;
    size_t data_off;        /* offset of first DSD byte in the file */
    size_t data_len;        /* bytes of DSD data (ch0_b0 ch1_b0 ... layout) */
    size_t pos;             /* current read offset into the DSD data region */
    int    channels;
};

static uint16_t rd_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t rd_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |
            (uint32_t)p[3];
}

static uint64_t rd_be64(const uint8_t *p)
{
    return ((uint64_t)rd_be32(p) << 32) | (uint64_t)rd_be32(p + 4);
}

static int dff_read(struct audio_source *src, void *buf, size_t bytes_per_ch)
{
    struct dff_state *st = src->opaque;
    const size_t want = bytes_per_ch * (size_t)st->channels;
    uint8_t *out = buf;
    size_t remaining = want;

    while (remaining > 0) {
        if (st->pos >= st->data_len) {
            st->pos = 0;  /* loop, matching WAV / DSF semantics */
        }
        size_t avail = st->data_len - st->pos;
        size_t take  = remaining < avail ? remaining : avail;
        memcpy(out, st->map + st->data_off + st->pos, take);
        st->pos    += take;
        out        += take;
        remaining  -= take;
    }
    return 0;
}

static void dff_close(struct audio_source *src)
{
    struct dff_state *st = src->opaque;
    munmap((void *)st->map, st->file_len);
    free(st);
    free(src);
}

/* Walk top-level sub-chunks of FRM8 looking for PROP and DSD. Returns 0 on
 * success with prop_off / prop_len and dsd_off / dsd_len filled, -1 on a
 * malformed file. The FRM8 container begins at `start` (offset 16 from
 * file start) and extends for `span` bytes. */
static int walk_frm8_children(const uint8_t *map, size_t map_len,
                              size_t start, size_t span,
                              size_t *prop_off, size_t *prop_len,
                              size_t *dsd_off,  size_t *dsd_len)
{
    size_t off = start;
    const size_t end = start + span;
    if (end > map_len) return -1;
    *prop_off = *prop_len = *dsd_off = *dsd_len = 0;

    while (off + 12 <= end) {
        const uint8_t *ck = map + off;
        const uint64_t ck_sz = rd_be64(ck + 4);
        if (ck_sz > end - off - 12) return -1;

        if (memcmp(ck, "PROP", 4) == 0) {
            *prop_off = off + 12;
            *prop_len = (size_t)ck_sz;
        } else if (memcmp(ck, "DSD ", 4) == 0) {
            *dsd_off = off + 12;
            *dsd_len = (size_t)ck_sz;
        } else if (memcmp(ck, "DST ", 4) == 0) {
            fprintf(stderr, "dff: DST-compressed files not supported "
                            "(decompression not implemented)\n");
            return -1;
        }
        /* FVER and other known chunks are skipped — we only need PROP+DSD. */

        /* Chunks are padded to an even byte count. */
        size_t advance = 12 + (size_t)ck_sz;
        if (advance & 1u) advance++;
        off += advance;
    }
    return 0;
}

/* Parse PROP/SND sub-chunks for sample rate and channel count. PROP's own
 * contents begin with a 4-byte property type ("SND ") followed by nested
 * sub-chunks with the same AIFF layout. */
static int parse_prop(const uint8_t *map, size_t prop_off, size_t prop_len,
                      uint32_t *out_rate, uint16_t *out_channels,
                      int *is_dsd_uncompressed)
{
    if (prop_len < 4) return -1;
    const uint8_t *prop = map + prop_off;
    if (memcmp(prop, "SND ", 4) != 0) {
        fprintf(stderr, "dff: PROP property type is not 'SND '\n");
        return -1;
    }

    *out_rate = 0;
    *out_channels = 0;
    *is_dsd_uncompressed = 0;

    size_t off = 4;
    while (off + 12 <= prop_len) {
        const uint8_t *ck = prop + off;
        const uint64_t ck_sz = rd_be64(ck + 4);
        if (ck_sz > prop_len - off - 12) return -1;

        if (memcmp(ck, "FS  ", 4) == 0) {
            if (ck_sz < 4) return -1;
            *out_rate = rd_be32(ck + 12);
        } else if (memcmp(ck, "CHNL", 4) == 0) {
            if (ck_sz < 2) return -1;
            *out_channels = rd_be16(ck + 12);
        } else if (memcmp(ck, "CMPR", 4) == 0) {
            /* Compression type: 4-byte ID + pstring description. */
            if (ck_sz < 4) return -1;
            *is_dsd_uncompressed = (memcmp(ck + 12, "DSD ", 4) == 0);
        }

        size_t advance = 12 + (size_t)ck_sz;
        if (advance & 1u) advance++;
        off += advance;
    }

    return 0;
}

struct audio_source *audio_source_dff_open(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("dff: open");
        return NULL;
    }
    struct stat sb;
    if (fstat(fd, &sb) < 0) {
        perror("dff: fstat");
        close(fd);
        return NULL;
    }
    const uint8_t *map = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        perror("dff: mmap");
        return NULL;
    }

    /* FRM8 chunk (outer container): 4B ID + 8B size + 4B form type. */
    if (sb.st_size < 16 || memcmp(map, "FRM8", 4) != 0) {
        fprintf(stderr, "dff: not a DSDIFF file (missing 'FRM8' magic)\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    const uint64_t frm8_size = rd_be64(map + 4);
    if (frm8_size < 4 || frm8_size > (uint64_t)sb.st_size - 12) {
        fprintf(stderr, "dff: FRM8 chunk size out of range\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    if (memcmp(map + 12, "DSD ", 4) != 0) {
        fprintf(stderr, "dff: FRM8 form type is not 'DSD '\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }

    /* Walk FRM8 children (starting after 4-byte form type) to locate PROP
     * and DSD. frm8_size covers the form type + child chunks. */
    const size_t children_off = 16;
    const size_t children_len = (size_t)frm8_size - 4;
    size_t prop_off, prop_len, dsd_off, dsd_len;
    if (walk_frm8_children(map, (size_t)sb.st_size,
                           children_off, children_len,
                           &prop_off, &prop_len,
                           &dsd_off, &dsd_len) < 0) {
        fprintf(stderr, "dff: malformed chunk structure\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    if (prop_off == 0 || dsd_off == 0) {
        fprintf(stderr, "dff: missing PROP or DSD chunk\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }

    uint32_t rate = 0;
    uint16_t channels = 0;
    int is_dsd_uncompressed = 0;
    if (parse_prop(map, prop_off, prop_len,
                   &rate, &channels, &is_dsd_uncompressed) < 0) {
        fprintf(stderr, "dff: malformed PROP chunk\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    if (!is_dsd_uncompressed) {
        fprintf(stderr, "dff: file is not uncompressed DSD "
                        "(CMPR must be 'DSD ')\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    if (channels < 1 || channels > 8) {
        fprintf(stderr, "dff: channel count %u out of range (1..8)\n",
                (unsigned)channels);
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    /* AOEther supports DSD64 through DSD2048 on the wire (DSD512+ via packet
     * splitting). These are the canonical DSDIFF sampling frequencies. */
    if (rate != 2822400u  && rate != 5644800u  &&
        rate != 11289600u && rate != 22579200u &&
        rate != 45158400u && rate != 90316800u) {
        fprintf(stderr, "dff: sampling frequency %u Hz not supported "
                        "(accepts DSD64/128/256/512/1024/2048)\n",
                rate);
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    if (dsd_off + dsd_len > (size_t)sb.st_size) {
        fprintf(stderr, "dff: truncated DSD data chunk\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    /* DSD data size must be a whole number of per-channel bytes. */
    if (dsd_len % (size_t)channels != 0) {
        fprintf(stderr, "dff: DSD data size %zu not a multiple of "
                        "channel count %u\n", dsd_len, (unsigned)channels);
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    if (dsd_len == 0) {
        fprintf(stderr, "dff: zero-length DSD stream\n");
        munmap((void *)map, sb.st_size);
        return NULL;
    }

    struct audio_source *src = calloc(1, sizeof(*src));
    struct dff_state    *st  = calloc(1, sizeof(*st));
    if (!src || !st) {
        free(src);
        free(st);
        munmap((void *)map, sb.st_size);
        return NULL;
    }
    st->map      = map;
    st->file_len = sb.st_size;
    st->data_off = dsd_off;
    st->data_len = dsd_len;
    st->pos      = 0;
    st->channels = (int)channels;

    src->read             = dff_read;
    src->close            = dff_close;
    src->channels         = (int)channels;
    src->rate             = (int)(rate / 8u);  /* DSD bytes/s/channel */
    src->bytes_per_sample = 1;
    src->opaque           = st;
    return src;
}
