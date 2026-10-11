/* modules/av/deint_test.c — deinterlacing and inverse telecine on synthetic
 * video: static detail must come out untouched (no shimmer), combing on moving
 * bars must go, hard-telecined film must come back as the exact film frames,
 * and the analysis must tell film from video. A real DVD file is checked when
 * HEMMA_TEST_MKV points at one. */
#include "../../tests/test.h"
#include "../../core/plat.h"
#include "deint.h"
#include "mkv.h"
#include "mpeg2.h"

#define W 96
#define H 64
#define CW (W / 2)
#define CH (H / 2)

typedef struct {
    uint8_t y[W * H], u[CW * CH], v[CW * CH];
} Img;

/* a film frame: a bright bar at x = pos on a textured background, plus a chroma bar */
static void film(Img *f, int pos) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int bg = 40 + ((x * 7 + y * 3) & 15);
            f->y[y * W + x] = (uint8_t)(x >= pos && x < pos + 20 ? 220 : bg);
        }
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) {
            f->u[y * CW + x] = (uint8_t)(x * 2 >= pos && x * 2 < pos + 20 ? 200 : 128);
            f->v[y * CW + x] = (uint8_t)(100 + (y & 3));
        }
}

/* even rows (luma and chroma) from t, odd rows from b */
static void weave(Img *o, const Img *t, const Img *b) {
    for (int y = 0; y < H; y++) memcpy(o->y + y * W, (y & 1 ? b : t)->y + y * W, W);
    for (int y = 0; y < CH; y++) {
        memcpy(o->u + y * CW, (y & 1 ? b : t)->u + y * CW, CW);
        memcpy(o->v + y * CW, (y & 1 ? b : t)->v + y * CW, CW);
    }
}

static DeintIn in_of(const Img *f, bool progressive, bool rff, int64_t pts) {
    return (DeintIn){f->y, f->u, f->v, W, CW, progressive, true, rff, pts};
}

static bool same(const DeintOut *o, const Img *f) {
    for (int y = 0; y < H; y++)
        if (memcmp(o->y + (size_t)y * o->stride_y, f->y + y * W, W)) return false;
    for (int y = 0; y < CH; y++)
        if (memcmp(o->u + (size_t)y * o->stride_c, f->u + y * CW, CW) || memcmp(o->v + (size_t)y * o->stride_c, f->v + y * CW, CW))
            return false;
    return true;
}

TEST(deint_leaves_static_detail_alone) {
    /* one-pixel horizontal lines: what every deinterlacer is tempted to smear */
    static Img f;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) f.y[y * W + x] = (uint8_t)(y & 1 ? 30 : 200 - x);
    memset(f.u, 128, sizeof f.u);
    memset(f.v, 128, sizeof f.v);
    for (int mode = DEINT_YADIF; mode <= DEINT_BOB; mode++) {
        Deint *d = deint_new(W, H, mode);
        CHECK(d);
        int got = 0, exact = 0;
        for (int i = 0; i < 6; i++) {
            DeintIn in = in_of(&f, false, false, i);
            int n = deint_push(d, &in);
            CHECK(n >= 0);
            DeintOut o;
            while (deint_pull(d, &o)) got++, exact += same(&o, &f);
        }
        deint_flush(d);
        DeintOut o;
        while (deint_pull(d, &o)) got++, exact += same(&o, &f);
        CHECK_INT(got, mode == DEINT_BOB ? 12 : 6);
        CHECK_INT(exact, got); /* nothing moves, so the missing field equals its neighbours in time */
        deint_free(d);
    }
}

TEST(deint_removes_combing_from_moving_video) {
    /* video-shot motion: every field shows the bar 8 px further on */
    static Img fields[24], frames[12];
    for (int i = 0; i < 24; i++) film(&fields[i], 4 + (i * 8) % (W - 30));
    for (int i = 0; i < 12; i++) weave(&frames[i], &fields[2 * i], &fields[2 * i + 1]);
    CHECK(deint_comb(frames[5].y, frames[5].y, W, W, H) > 80); /* the input is combed */
    DeintStats st = {0};
    for (int i = 0; i < 12; i++) {
        DeintIn in = in_of(&frames[i], false, false, i);
        deint_analyze(&st, &in, W, H);
    }
    CHECK_INT(deint_kind(&st), DEINT_KIND_INTERLACED);
    for (int mode = DEINT_YADIF; mode <= DEINT_BOB; mode++) {
        Deint *d = deint_new(W, H, mode);
        int got = 0, worst = 0;
        for (int i = 0; i < 12; i++) {
            DeintIn in = in_of(&frames[i], false, false, i * 1001);
            deint_push(d, &in);
            DeintOut o;
            while (deint_pull(d, &o)) {
                int c = deint_comb(o.y, o.y, o.stride_y, W, H);
                if (c > worst) worst = c;
                CHECK(o.interpolated);
                got++;
            }
        }
        deint_flush(d);
        DeintOut o;
        while (deint_pull(d, &o)) got++;
        CHECK_INT(got, mode == DEINT_BOB ? 24 : 12);
        CHECK(worst < 8); /* no combing left */
        deint_free(d);
    }
    int num, den;
    deint_rate(DEINT_BOB, 30000, 1001, &num, &den);
    CHECK(num == 60000 && den == 1001);
    deint_rate(DEINT_IVTC, 30000, 1001, &num, &den);
    CHECK(num == 24000 && den == 1001);
    deint_rate(DEINT_YADIF, 25, 1, &num, &den);
    CHECK(num == 25 && den == 1);
}

TEST(deint_ivtc_gives_back_the_film_frames) {
    /* 12 film frames through 3:2 pulldown (TFF): A B C D -> At+Ab Bt+Bb Bt+Cb Ct+Db Dt+Db */
    enum { NF = 12, NV = NF / 4 * 5 };
    static Img film_frames[NF], video[NV];
    for (int i = 0; i < NF; i++) film(&film_frames[i], 2 + i * 6);
    for (int c = 0; c < NF / 4; c++) {
        const Img *a = &film_frames[4 * c], *b = a + 1, *cc = a + 2, *dd = a + 3;
        Img *v = &video[5 * c];
        weave(&v[0], a, a);
        weave(&v[1], b, b);
        weave(&v[2], b, cc);
        weave(&v[3], cc, dd);
        weave(&v[4], dd, dd);
    }
    CHECK(deint_comb(video[2].y, video[2].y, W, W, H) > 80);
    CHECK(deint_comb(video[0].y, video[0].y, W, W, H) == 0);
    DeintStats st = {0};
    for (int i = 0; i < NV; i++) {
        DeintIn in = in_of(&video[i], false, false, i);
        deint_analyze(&st, &in, W, H);
    }
    CHECK_INT(deint_kind(&st), DEINT_KIND_HARD_TELECINE);
    CHECK_INT(deint_mode_for(DEINT_KIND_HARD_TELECINE), DEINT_IVTC);

    Deint *d = deint_new(W, H, DEINT_IVTC);
    int got = 0;
    for (int i = 0; i < NV; i++) {
        DeintIn in = in_of(&video[i], false, false, i * 1001);
        CHECK(deint_push(d, &in) >= 0);
        DeintOut o;
        while (deint_pull(d, &o)) {
            CHECK(got < NF);
            CHECK(!o.interpolated);
            if (!same(&o, &film_frames[got])) {
                test_fail(__FILE__, __LINE__, "output %d is not film frame %d", got, got);
                deint_free(d);
                return;
            }
            CHECK_INT(o.index, got);
            got++;
        }
    }
    deint_flush(d);
    DeintOut o;
    while (deint_pull(d, &o)) {
        CHECK(got < NF && same(&o, &film_frames[got]));
        got++;
    }
    CHECK_INT(got, NF);
    const DeintCounters *k = deint_counters(d);
    CHECK_INT(k->dropped, NF / 4);
    CHECK_INT(k->interpolated, 0);
    CHECK_INT(k->match_p, 2 * (NF / 4)); /* Bt+Cb and Ct+Db find their partner in the frame before */
    deint_free(d);
}

TEST(deint_passes_progressive_and_soft_telecine_through) {
    static Img f[6];
    for (int i = 0; i < 6; i++) film(&f[i], 3 + i * 9);
    DeintStats st = {0};
    Deint *d = deint_new(W, H, DEINT_PASS);
    int got = 0, threes = 0;
    for (int i = 0; i < 6; i++) {
        DeintIn in = in_of(&f[i], true, i & 1, i);
        deint_analyze(&st, &in, W, H);
        deint_push(d, &in);
        DeintOut o;
        while (deint_pull(d, &o)) {
            CHECK(same(&o, &f[got]) && !o.interpolated);
            threes += o.fields == 3;
            got++;
        }
    }
    deint_flush(d);
    DeintOut o;
    while (deint_pull(d, &o)) {
        CHECK(same(&o, &f[got]));
        threes += o.fields == 3;
        got++;
    }
    CHECK_INT(got, 6);
    CHECK_INT(threes, 3); /* RFF frames last three fields: 2-3-2-3 = 23.976 fps in a 29.97 stream */
    CHECK_INT(deint_kind(&st), DEINT_KIND_SOFT_TELECINE);
    deint_free(d);
    DeintStats prog = {0};
    DeintIn in = in_of(&f[0], true, false, 0);
    for (int i = 0; i < 20; i++) deint_analyze(&prog, &in, W, H);
    CHECK_INT(deint_kind(&prog), DEINT_KIND_PROGRESSIVE);
    CHECK(deint_new(2, 2, DEINT_YADIF) == NULL);
    CHECK(deint_new(W, H, 7) == NULL);
}

/* ------------------------------------------------------- a real DVD rip */

static bool rd(void *ctx, int64_t off, void *dst, size_t n) { return file_pread(ctx, off, dst, n); }

TEST(deint_real_dvd_when_given) {
    const char *path = plat_getenv("HEMMA_TEST_MKV");
    if (!path || !*path) return;
    int64_t size = 0;
    CHECK(path_stat(path, &size, NULL, NULL));
    FILE *fp = file_open(path, "rb");
    MkvReader r = {fp, size, rd};
    char err[128];
    Mkv *m = mkv_open(&r, err, sizeof err);
    CHECK(m);
    const MkvTrack *vt = mkv_find_track(m, MKV_VIDEO);
    if (!vt || strcmp(vt->codec, "V_MPEG2") != 0) {
        mkv_close(m);
        fclose(fp);
        return;
    }
    Mpeg2 *dec = mpeg2_new();
    if (vt->priv) mpeg2_decode(dec, vt->priv, vt->priv_len, MPEG2_NOPTS);
    mkv_seek(m, mkv_duration(m) / 3, 0);
    Deint *iv = NULL, *ya = NULL;
    DeintStats st = {0};
    int frames = 0, out_iv = 0, out_ya = 0, combed_iv = 0;
    MkvFrame fr;
    bool key = false;
    while (frames < 600 && mkv_next(m, &fr) == 1) {
        if (fr.track != vt->number || (!key && !(key = fr.key))) continue;
        mpeg2_decode(dec, fr.data, fr.size, fr.pts);
        Mpeg2Frame f;
        while (mpeg2_frame(dec, &f) && frames < 600) {
            if (!iv) {
                iv = deint_new(f.width, f.height, DEINT_IVTC);
                ya = deint_new(f.width, f.height, DEINT_YADIF);
            }
            DeintIn in = {f.y, f.u, f.v, f.stride_y, f.stride_c, f.progressive_frame, f.top_field_first, f.repeat_first_field, f.pts};
            deint_analyze(&st, &in, f.width, f.height);
            deint_push(iv, &in);
            deint_push(ya, &in);
            DeintOut o;
            while (deint_pull(iv, &o)) out_iv++, combed_iv += deint_comb(o.y, o.y, o.stride_y, o.width, o.height) > 80;
            while (deint_pull(ya, &o)) out_ya++;
            frames++;
        }
    }
    CHECK(iv && ya);
    deint_flush(iv);
    deint_flush(ya);
    DeintOut o;
    while (deint_pull(iv, &o)) out_iv++;
    while (deint_pull(ya, &o)) out_ya++;
    CHECK_INT(out_ya, frames);
    CHECK(out_iv >= frames * 4 / 5 - 1 && out_iv <= frames * 4 / 5 + 1);
    CHECK(combed_iv * 50 < out_iv); /* < 2% of the IVTC output still looks combed */
    printf("    real DVD: %s; %d frames -> IVTC %d (c %lld p %lld n %lld, yadif %lld), combed after %d\n",
           deint_kind_name(deint_kind(&st)), frames, out_iv, (long long)deint_counters(iv)->match_c,
           (long long)deint_counters(iv)->match_p, (long long)deint_counters(iv)->match_n,
           (long long)deint_counters(iv)->interpolated, combed_iv);
    deint_free(iv);
    deint_free(ya);
    mpeg2_free(dec);
    mkv_close(m);
    fclose(fp);
}

TEST(deint_spots_progressive_pictures_in_interlaced_frames) {
    /* a 29.97p show stored as interlaced frames (both fields from one instant):
       it moves, yet nothing is ever combed - keep the pictures as they are */
    enum { N = 30 };
    static Img f[N];
    for (int i = 0; i < N; i++) film(&f[i], 2 + (i * 5) % (W - 30));
    DeintStats st = {0};
    for (int i = 0; i < N; i++) {
        DeintIn in = in_of(&f[i], false, false, i);
        deint_analyze(&st, &in, W, H);
    }
    CHECK_INT(st.combed, 0);
    CHECK(st.moving >= N - 2);
    CHECK_INT(deint_kind(&st), DEINT_KIND_PROGRESSIVE);
    CHECK_INT(deint_mode_for(deint_kind(&st)), DEINT_PASS);
    Deint *d = deint_new(W, H, DEINT_PASS);
    int got = 0;
    for (int i = 0; i <= N; i++) {
        if (i < N) {
            DeintIn in = in_of(&f[i], false, false, i);
            deint_push(d, &in);
        } else deint_flush(d);
        DeintOut o;
        while (deint_pull(d, &o)) {
            CHECK(!o.interpolated && same(&o, &f[got]));
            got++;
        }
    }
    CHECK_INT(got, N);
    deint_free(d);
}
