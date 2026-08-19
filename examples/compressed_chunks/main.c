/*
 * © 2026 Erik Carstensen
 * SPDX-License-Identifier: MPL-2.0
 *
 * Example: lazily mmap the *uncompressed* content of a file that stores its
 * chunks independently deflated (see scripts/gen_compressed_fixture.py).
 * Each chunk is only decompressed the first time it is actually read.
 */
#include "faultcache/faultcache.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

struct chunk_info {
    uint64_t uncompressed_size;
    uint64_t compressed_size;
    uint64_t compressed_offset;
};

struct fixture {
    uint8_t *data; /* whole file, owned */
    size_t data_len;
    uint32_t nchunks;
    struct chunk_info *chunks;
    const uint8_t *blob; /* points into data */
};

static uint64_t read_u64le(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static uint32_t read_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int fixture_load(const char *path, struct fixture *fx) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("fopen");
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 8) {
        fclose(f);
        return -1;
    }

    fx->data = malloc((size_t)len);
    if (!fx->data || fread(fx->data, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        return -1;
    }
    fclose(f);
    fx->data_len = (size_t)len;

    if (memcmp(fx->data, "FCFX", 4) != 0) {
        fprintf(stderr, "bad magic\n");
        return -1;
    }
    fx->nchunks = read_u32le(fx->data + 4);

    const uint8_t *hdr = fx->data + 8;
    fx->chunks = malloc(fx->nchunks * sizeof(*fx->chunks));
    for (uint32_t i = 0; i < fx->nchunks; i++) {
        fx->chunks[i].uncompressed_size = read_u64le(hdr + 0);
        fx->chunks[i].compressed_size = read_u64le(hdr + 8);
        fx->chunks[i].compressed_offset = read_u64le(hdr + 16);
        hdr += 24;
    }
    fx->blob = hdr;
    return 0;
}

static void init_chunk(uint32_t chunk, void *start, size_t size,
                        void *user_data) {
    struct fixture *fx = user_data;
    const struct chunk_info *ci = &fx->chunks[chunk];
    if (ci->uncompressed_size != size) {
        fprintf(stderr, "chunk %u: size mismatch\n", chunk);
        abort();
    }

    uLongf dest_len = (uLongf)size;
    int rc = uncompress((Bytef *)start, &dest_len, fx->blob + ci->compressed_offset,
                         (uLong)ci->compressed_size);
    if (rc != Z_OK || dest_len != size) {
        fprintf(stderr, "chunk %u: decompression failed (rc=%d)\n", chunk, rc);
        abort();
    }
}

static uint8_t expected_byte(uint32_t chunk, size_t i) {
    return (uint8_t)((chunk * 131u + (uint32_t)i * 7u) & 0xFF);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s FIXTURE\n", argv[0]);
        return 1;
    }

    struct fixture fx = {0};
    if (fixture_load(argv[1], &fx) != 0)
        return 1;

    fc_pool_t *pool = fc_pool_create();
    if (!pool) {
        perror("fc_pool_create");
        return 1;
    }

    size_t *sizes = malloc(fx.nchunks * sizeof(size_t));
    for (uint32_t i = 0; i < fx.nchunks; i++)
        sizes[i] = (size_t)fx.chunks[i].uncompressed_size;

    fc_region_t *region = fc_region_create(pool, fx.nchunks, sizes, init_chunk, &fx);
    if (!region) {
        perror("fc_region_create");
        return 1;
    }
    const void *base = fc_region_base(region);

    size_t *offsets = malloc(fx.nchunks * sizeof(size_t));
    offsets[0] = 0;
    for (uint32_t i = 1; i < fx.nchunks; i++)
        offsets[i] = offsets[i - 1] + sizes[i - 1];

    /* Touch chunks out of order to demonstrate random access. */
    const unsigned char *p = base;
    for (uint32_t k = 0; k < fx.nchunks; k++) {
        uint32_t chunk = (fx.nchunks - 1) - k;
        for (size_t i = 0; i < sizes[chunk]; i++) {
            unsigned char got = p[offsets[chunk] + i];
            unsigned char want = expected_byte(chunk, i);
            if (got != want) {
                fprintf(stderr, "chunk %u byte %zu: got %u want %u\n", chunk,
                        i, got, want);
                return 1;
            }
        }
    }

    printf("ok: %u chunks, %zu bytes total, all lazily decompressed and "
           "verified\n",
           fx.nchunks, offsets[fx.nchunks - 1] + sizes[fx.nchunks - 1]);

    fc_region_destroy(region);
    fc_pool_destroy(pool);
    free(sizes);
    free(offsets);
    free(fx.chunks);
    free(fx.data);
    return 0;
}
