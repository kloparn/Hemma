/* tools/convfuzz.c — fuzzing modules/av's converter: av_convert on MKV bytes in memory, the MP4
 * written to memory. Reaches the demuxer, the MPEG-2 decoder, the deinterlacer, the H.264
 * encoder, the AC-3 decoder, the AAC encoder and the MP4 writer (dev aid, not part of the app).
 *
 * libFuzzer (clang on Linux/macOS; CI's fuzz job does this):
 *   HEMMA_FUZZ_SEEDS=build/fuzz-seeds build/test conv_fuzz_seeds      the seed files (made by
 *                                        the converter tests: small MKVs of every kind it takes)
 *   clang -g -O1 -std=gnu11 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
 *         -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION tools/convfuzz.c modules/av/conv.c modules/av/mkv.c \
 *         modules/av/mpeg2.c modules/av/deint.c modules/av/h264enc.c modules/av/ac3.c modules/av/aac.c \
 *         modules/av/mp4.c -lm -o build/convfuzz
 *   build/convfuzz -max_total_time=90 -max_len=65536 -rss_limit_mb=2048 -timeout=60 build/fuzz-corpus build/fuzz-seeds
 *
 * Replay, anywhere (zig cc on Windows too): -DCONVFUZZ_MAIN gives a plain main() that runs each
 * file named, and with --mutate N also N damaged copies of each (bit flips, cuts, garbage, runs
 * copied, dropped or repeated, stray start codes - like avdump fuzz), the same ones for the same --seed:
 *   zig cc -g -O1 -std=gnu11 -fsanitize=undefined -fsanitize-trap=undefined -DCONVFUZZ_MAIN \
 *          -DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION tools/convfuzz.c modules/av/conv.c ... -o build/convfuzz.exe
 *   build/convfuzz.exe FILE... [--mutate N] [--seed S] [--last FILE]      (e.g. every seed file, from bash)
 *   (--last: each input is written there before it runs - after a crash, that's the culprit)
 *
 * FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION turns the AC-3 CRC check off (conv.c), so mutated
 * Dolby frames reach the decoder - a crafted file can carry good CRCs anyway.
 *
 * What counts as a finding: a sanitizer report (crash, overflow, undefined behaviour), a
 * leak, a hang, or the converter breaking its own promises - checked below: a success writes
 * a whole MP4 (top-level boxes back to back, exactly out_bytes long), a failure says why. */
#include "../modules/av/conv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OUT_MAX (64u << 20)      /* the "disk": a bigger copy fails as a full one would */
#define MAX_PIXELS (352 * 288)   /* MPEG-2 announcing more is skipped: right, just slow to encode under sanitizers */
#define MAX_SAMPLES_EACH 256     /* per track (AvConvJob.max_samples): small inputs need no more, and it bounds the work */

typedef struct {
    const uint8_t *p;
    size_t n;
} In;

typedef struct {
    uint8_t *p;
    size_t len, cap;
} Out;

static bool in_read(void *ctx, int64_t off, void *dst, size_t n) {
    const In *in = ctx;
    if (off < 0 || (uint64_t)off > in->n || n > in->n - (size_t)off) return false;
    if (n) memcpy(dst, in->p + off, n);
    return true;
}

static bool out_write(void *ctx, const void *p, size_t n) {
    Out *o = ctx;
    if (n > OUT_MAX - o->len) return false;
    if (o->len + n > o->cap) {
        size_t cap = o->cap ? o->cap : 65536;
        while (cap < o->len + n) cap *= 2;
        if (cap > OUT_MAX) cap = OUT_MAX;
        uint8_t *q = realloc(o->p, cap);
        if (!q) return false;
        o->p = q, o->cap = cap;
    }
    if (n) memcpy(o->p + o->len, p, n);
    o->len += n;
    return true;
}

static bool out_write_at(void *ctx, int64_t off, const void *p, size_t n) {
    Out *o = ctx;
    if (off < 0 || (uint64_t)off > o->len || n > o->len - (size_t)off) return false;
    if (n) memcpy(o->p + off, p, n);
    return true;
}

/* An MPEG-2 sequence header asking for a picture over MAX_PIXELS anywhere in the bytes (the
   decoder takes up to 4096x4096: correct, but seconds a frame to encode under ASan). */
static bool too_big(const uint8_t *p, size_t n) {
    for (size_t i = 0; i + 7 <= n; i++) {
        if (p[i] || p[i + 1] || p[i + 2] != 1 || p[i + 3] != 0xB3) continue;
        unsigned w = (unsigned)p[i + 4] << 4 | p[i + 5] >> 4, h = (unsigned)(p[i + 5] & 15) << 8 | p[i + 6];
        if ((unsigned long)w * h > MAX_PIXELS) return true;
    }
    return false;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* The output is an MP4: ftyp first, then boxes back to back up to the very end. */
static bool whole_mp4(const Out *o) {
    if (o->len < 16 || memcmp(o->p + 4, "ftyp", 4)) return false;
    for (size_t at = 0; at < o->len;) {
        if (o->len - at < 8) return false;
        uint64_t size = be32(o->p + at);
        if (size == 1) { /* 64-bit size (the mdat) */
            if (o->len - at < 16) return false;
            size = (uint64_t)be32(o->p + at + 8) << 32 | be32(o->p + at + 12);
        }
        if (size < 8 || size > o->len - at) return false;
        at += (size_t)size;
    }
    return true;
}

typedef struct {
    long runs, ok, refused, skipped;
} Tally;

static int convert(const uint8_t *data, size_t size, Tally *t) {
    if (too_big(data, size)) {
        if (t) t->skipped++;
        return 0;
    }
    In in = {data, size};
    Out out = {0};
    AvConvJob j;
    memset(&j, 0, sizeof j);
    j.in = (MkvReader){&in, (int64_t)size, in_read};
    j.out = (Mp4Out){&out, out_write, out_write_at};
    j.max_samples = MAX_SAMPLES_EACH;
    j.want_subs = true; /* text subtitle tracks too */
    bool ok = av_convert(&j);
    if (ok && (j.err[0] || j.permille != 1000 || j.out_bytes != (int64_t)out.len || !whole_mp4(&out))) {
        fprintf(stderr, "convfuzz: a success that didn't write a whole MP4 (%lld bytes reported, %zu written)\n",
                (long long)j.out_bytes, out.len);
        abort();
    }
    if (!ok && !j.err[0]) {
        fprintf(stderr, "convfuzz: failed without saying why\n");
        abort();
    }
    for (int k = 0; k < j.nsubs; k++)
        if (!j.subs[k].srt || strlen(j.subs[k].srt) != j.subs[k].len || j.subs[k].len > AV_CONV_SUB_BYTES) {
            fprintf(stderr, "convfuzz: subtitle track %d isn't one NUL-terminated SRT text within its bound\n", k);
            abort();
        }
    av_convert_free(&j);
    if (t) {
        t->runs++;
        if (ok) t->ok++;
        else t->refused++;
    }
    free(out.p);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) { return convert(data, size, NULL); }

#ifdef CONVFUZZ_MAIN
/* ------------------------------------------------------------- replay + mutate */

static uint32_t rng;
static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

/* 1-8 kinds of damage to buf[0..n) (room for cap bytes); returns the new length */
static size_t damage(uint8_t *buf, size_t n, size_t cap) {
    for (int ops = 1 + (int)(rnd() % 8); ops > 0 && n > 0; ops--) {
        size_t at = rnd() % n, len = 1 + rnd() % 64;
        switch (rnd() % 9) {
        case 0:
        case 1: /* a few bit flips */
            for (int k = 1 + (int)(rnd() % 8); k > 0; k--) buf[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8));
            break;
        case 2: n = at; break; /* cut short */
        case 3: /* a run of garbage */
            for (size_t k = at; k < n && k < at + len; k++) buf[k] = (uint8_t)rnd();
            break;
        case 4: { /* a byte that means something somewhere: sizes, flags, counts */
            static const uint8_t v[] = {0x00, 0x01, 0x7F, 0x80, 0xFF, 0xFE, 0x40, 0x08};
            buf[at] = v[rnd() % sizeof v];
            break;
        }
        case 5: /* a run copied over another place */
            if (len > n - at) len = n - at;
            memmove(buf + rnd() % (n - len + 1), buf + at, len);
            break;
        case 6: /* a run dropped */
            if (len > n - at) len = n - at;
            memmove(buf + at, buf + at + len, n - at - len);
            n -= len;
            break;
        case 7: /* an MPEG-2 start code (sequence, extension, GOP, picture, slice, end) or a Dolby sync word */
            if (n - at >= 4) {
                static const uint8_t code[] = {0xB3, 0xB5, 0xB8, 0x00, 0x01, 0x2F, 0xB7, 0xB2};
                uint32_t r = rnd();
                if (r % 5 == 0) buf[at] = 0x0B, buf[at + 1] = 0x77;
                else buf[at] = buf[at + 1] = 0, buf[at + 2] = 1, buf[at + 3] = code[r % sizeof code];
            }
            break;
        default: /* a run repeated (inserted): sizes stop adding up */
            if (len > n - at) len = n - at;
            if (n + len <= cap) {
                memmove(buf + at + len, buf + at, n - at);
                n += len;
            }
        }
    }
    return n;
}

static uint8_t *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *p = NULL;
    size_t cap = 0;
    *n = 0;
    for (;;) {
        if (*n == cap) {
            uint8_t *q = realloc(p, cap = cap ? cap * 2 : 65536);
            if (!q) break;
            p = q;
        }
        size_t got = fread(p + *n, 1, cap - *n, f);
        if (!got) break;
        *n += got;
    }
    fclose(f);
    return p;
}

int main(int argc, char **argv) {
    long mutate = 0;
    unsigned long seed = 1;
    const char *last = NULL;
    int files = 0;
    Tally t = {0};
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mutate") && i + 1 < argc) mutate = atol(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) seed = strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--last") && i + 1 < argc) last = argv[++i];
    }
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--", 2)) {
            i++;
            continue;
        }
        size_t n = 0;
        uint8_t *orig = read_file(argv[i], &n);
        if (!orig) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            return 1;
        }
        files++;
        size_t cap = n + 4096;
        uint8_t *buf = malloc(cap);
        if (!buf) return 1;
        for (long k = -1; k < mutate; k++) { /* k = -1: the file as it is */
            size_t len = n;
            memcpy(buf, orig, n);
            if (k >= 0) {
                rng = (uint32_t)(seed * 2654435761u) ^ (uint32_t)(k * 40503u + (unsigned long)files * 977u) ^ 0x9E3779B9u;
                if (!rng) rng = 1;
                len = damage(buf, len, cap);
            }
            if (last) {
                FILE *f = fopen(last, "wb");
                if (f) fwrite(buf, 1, len, f), fclose(f);
            }
            convert(buf, len, &t);
        }
        free(buf);
        free(orig);
        fprintf(stderr, "%s: done\n", argv[i]);
    }
    printf("convfuzz: %d files, %ld runs: %ld converted, %ld refused, %ld skipped (pictures too big) - no crash\n", files, t.runs,
           t.ok, t.refused, t.skipped);
    return 0;
}
#endif
