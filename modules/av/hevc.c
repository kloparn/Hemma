/* modules/av/hevc.c — HEVC decoder: NAL units, parameter sets, slice headers, reference pictures and the
   order pictures come out in. The coding tree is hevc_ctu.c's, the sample work hevc_dsp.c's. See hevc.h. */
#include "hevc_int.h"

/* ---- reading bits of an RBSP (emulation prevention already taken out) ---- */

typedef struct {
    const uint8_t *p;
    size_t n, pos; /* pos in bits */
    bool err;
} Br;

static uint32_t br_u(Br *b, int k) {
    uint32_t v = 0;
    for (int i = 0; i < k; i++) {
        size_t byte = b->pos >> 3;
        if (byte >= b->n) {
            b->err = true;
            return 0;
        }
        v = (v << 1) | ((b->p[byte] >> (7 - (b->pos & 7))) & 1);
        b->pos++;
    }
    return v;
}

static uint32_t br_ue(Br *b) {
    int zeros = 0;
    while (!br_u(b, 1)) {
        if (b->err || ++zeros > 31) {
            b->err = true;
            return 0;
        }
    }
    return zeros ? ((1u << zeros) - 1) + br_u(b, zeros) : 0;
}

static int32_t br_se(Br *b) {
    uint32_t k = br_ue(b);
    return (k & 1) ? (int32_t)((k + 1) >> 1) : -(int32_t)(k >> 1);
}

/* an ue(v) that must lie in [lo, hi]: out of it marks the reader broken */
static int br_uer(Br *b, int lo, int hi) {
    uint32_t v = br_ue(b);
    if (v > (uint32_t)hi || (int)v < lo) b->err = true;
    return b->err ? lo : (int)v;
}

static int br_ser(Br *b, int lo, int hi) {
    int32_t v = br_se(b);
    if (v < lo || v > hi) b->err = true;
    return b->err ? 0 : v;
}

static int ceil_log2(int v) {
    int k = 0;
    while ((1 << k) < v) k++;
    return k;
}

/* ---- parameter sets ---- */

static void ptl(Br *b, int sub_layers) { /* profile_tier_level(1, sps_max_sub_layers_minus1): nothing kept */
    br_u(b, 8), br_u(b, 32), br_u(b, 32), br_u(b, 16), br_u(b, 8); /* profile, compatibility, 48 flag bits, level */
    bool prof[8] = {0}, lev[8] = {0};
    for (int i = 0; i < sub_layers; i++) prof[i] = br_u(b, 1), lev[i] = br_u(b, 1);
    if (sub_layers > 0)
        for (int i = sub_layers; i < 8; i++) br_u(b, 2);
    for (int i = 0; i < sub_layers; i++) {
        if (prof[i]) br_u(b, 8), br_u(b, 32), br_u(b, 32), br_u(b, 16);
        if (lev[i]) br_u(b, 8);
    }
}

static const uint8_t DEFAULT_8x8[2][64] = {
    {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18, 17, 18, 18, 17, 18, 21,
     19, 20, 21, 20, 19, 21, 24, 22, 22, 24, 24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29,
     31, 35, 35, 31, 29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115},
    {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 20,
     20, 20, 20, 20, 20, 20, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28,
     28, 28, 28, 28, 28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91}};

static void scaling_default(HvScaling *sl) {
    for (int m = 0; m < 6; m++) {
        memset(sl->list[0][m], 16, 16);
        for (int s = 1; s < 4; s++) memcpy(sl->list[s][m], DEFAULT_8x8[m >= 3], 64), sl->dc[s][m] = 16;
    }
}

static void scaling_data(Br *b, HvScaling *sl) {
    for (int s = 0; s < 4; s++)
        for (int m = 0; m < 6; m += (s == 3) ? 3 : 1) {
            int n = s ? 64 : 16;
            if (!br_u(b, 1)) { /* predicted from a list before it, or the default */
                int delta = br_uer(b, 0, s == 3 ? m / 3 : m);
                if (!delta) {
                    if (s) memcpy(sl->list[s][m], DEFAULT_8x8[m >= 3], 64), sl->dc[s][m] = 16;
                    else memset(sl->list[0][m], 16, 16);
                } else {
                    int r = m - delta * (s == 3 ? 3 : 1);
                    memcpy(sl->list[s][m], sl->list[s][r], (size_t)n);
                    sl->dc[s][m] = sl->dc[s][r];
                }
            } else {
                int next = 8;
                if (s > 1) next = br_ser(b, -7, 247) + 8, sl->dc[s][m] = (uint8_t)next;
                for (int i = 0; i < n; i++) {
                    next = (next + br_ser(b, -128, 127) + 256) % 256;
                    sl->list[s][m][i] = (uint8_t)next;
                }
            }
            if (s == 3) /* 32x32 chroma (4:4:4 only): as 16x16's - kept whole for the copies above */
                for (int k = 1; k < 3; k++)
                    memcpy(sl->list[3][m + k], sl->list[2][m + k], 64), sl->dc[3][m + k] = sl->dc[2][m + k];
        }
}

static bool st_rps(Br *b, HvRps *out, const HvRps *sets, int idx, int nsets, bool in_slice) {
    memset(out, 0, sizeof *out);
    bool pred = idx ? br_u(b, 1) : false;
    if (pred) {
        int delta_idx = in_slice ? br_uer(b, 0, idx - 1) + 1 : 1;
        if (b->err || delta_idx > idx) return false;
        const HvRps *r = &sets[idx - delta_idx];
        int sign = (int)br_u(b, 1), abs = br_uer(b, 0, 32767) + 1;
        int drps = sign ? -abs : abs, nr = r->nneg + r->npos;
        uint8_t used[HV_MAX_REF + 1], use[HV_MAX_REF + 1];
        for (int j = 0; j <= nr; j++) {
            used[j] = (uint8_t)br_u(b, 1);
            use[j] = used[j] ? 1 : (uint8_t)br_u(b, 1);
        }
        int32_t s0[HV_MAX_REF * 2 + 1], s1[HV_MAX_REF * 2 + 1];
        uint8_t u0[HV_MAX_REF * 2 + 1], u1[HV_MAX_REF * 2 + 1];
        int i = 0;
        for (int j = r->npos - 1; j >= 0; j--) {
            int dpoc = r->delta[r->nneg + j] + drps;
            if (dpoc < 0 && use[r->nneg + j]) s0[i] = dpoc, u0[i++] = used[r->nneg + j];
        }
        if (drps < 0 && use[nr]) s0[i] = drps, u0[i++] = used[nr];
        for (int j = 0; j < r->nneg; j++) {
            int dpoc = r->delta[j] + drps;
            if (dpoc < 0 && use[j]) s0[i] = dpoc, u0[i++] = used[j];
        }
        int nneg = i;
        i = 0;
        for (int j = r->nneg - 1; j >= 0; j--) {
            int dpoc = r->delta[j] + drps;
            if (dpoc > 0 && use[j]) s1[i] = dpoc, u1[i++] = used[j];
        }
        if (drps > 0 && use[nr]) s1[i] = drps, u1[i++] = used[nr];
        for (int j = 0; j < r->npos; j++) {
            int dpoc = r->delta[r->nneg + j] + drps;
            if (dpoc > 0 && use[r->nneg + j]) s1[i] = dpoc, u1[i++] = used[r->nneg + j];
        }
        if (nneg + i > HV_MAX_REF) return false;
        out->nneg = nneg, out->npos = i;
        for (int k = 0; k < nneg; k++) out->delta[k] = s0[k], out->used[k] = u0[k];
        for (int k = 0; k < i; k++) out->delta[nneg + k] = s1[k], out->used[nneg + k] = u1[k];
    } else {
        int nneg = br_uer(b, 0, HV_MAX_REF), npos = br_uer(b, 0, HV_MAX_REF);
        if (b->err || nneg + npos > HV_MAX_REF) return false;
        int32_t poc = 0;
        for (int k = 0; k < nneg; k++) {
            poc -= br_uer(b, 0, 32767) + 1;
            out->delta[k] = poc, out->used[k] = (uint8_t)br_u(b, 1);
        }
        poc = 0;
        for (int k = 0; k < npos; k++) {
            poc += br_uer(b, 0, 32767) + 1;
            out->delta[nneg + k] = poc, out->used[nneg + k] = (uint8_t)br_u(b, 1);
        }
        out->nneg = nneg, out->npos = npos;
    }
    (void)nsets;
    return !b->err;
}

static void sub_hrd(Br *b, int cpb, bool sub_pic) {
    for (int i = 0; i <= cpb; i++) {
        br_ue(b), br_ue(b);
        if (sub_pic) br_ue(b), br_ue(b);
        br_u(b, 1);
    }
}

static void hrd(Br *b, int sub_layers) {
    bool nal = br_u(b, 1), vcl = br_u(b, 1), sub_pic = false;
    if (nal || vcl) {
        sub_pic = br_u(b, 1);
        if (sub_pic) br_u(b, 8), br_u(b, 5), br_u(b, 1), br_u(b, 5);
        br_u(b, 8);
        if (sub_pic) br_u(b, 4);
        br_u(b, 15);
    }
    for (int i = 0; i <= sub_layers && !b->err; i++) {
        bool fixed = br_u(b, 1), within = fixed ? true : br_u(b, 1), low_delay = false;
        if (within) br_ue(b);
        else low_delay = br_u(b, 1);
        int cpb = low_delay ? 0 : br_uer(b, 0, 31);
        if (nal) sub_hrd(b, cpb, sub_pic);
        if (vcl) sub_hrd(b, cpb, sub_pic);
    }
}

static const uint8_t SAR[17][2] = {{0, 0},   {1, 1},   {12, 11}, {10, 11}, {16, 11}, {40, 33},
                                   {24, 11}, {20, 11}, {32, 11}, {80, 33}, {18, 11}, {15, 11},
                                   {64, 33}, {160, 99}, {4, 3},  {3, 2},   {2, 1}};

static void vui(Br *b, HvSps *s, int sub_layers) {
    if (br_u(b, 1)) {
        int idc = (int)br_u(b, 8);
        if (idc == 255) s->sar_num = (int)br_u(b, 16), s->sar_den = (int)br_u(b, 16);
        else if (idc < 17) s->sar_num = SAR[idc][0], s->sar_den = SAR[idc][1];
    }
    if (br_u(b, 1)) br_u(b, 1);
    if (br_u(b, 1)) {
        br_u(b, 4);
        if (br_u(b, 1)) br_u(b, 24);
    }
    if (br_u(b, 1)) br_ue(b), br_ue(b);
    br_u(b, 1);
    if (br_u(b, 1)) s->unsup = "interlaced pictures sent as fields"; /* field_seq_flag */
    br_u(b, 1);
    if (br_u(b, 1)) br_ue(b), br_ue(b), br_ue(b), br_ue(b);
    if (br_u(b, 1)) {
        uint32_t tick = br_u(b, 32), scale = br_u(b, 32);
        if (tick && scale && scale <= 0x7fffffff && tick <= 0x7fffffff) s->rate_num = (int)scale, s->rate_den = (int)tick;
        if (br_u(b, 1)) br_ue(b);
        if (br_u(b, 1)) hrd(b, sub_layers);
    }
    if (br_u(b, 1)) br_u(b, 3), br_ue(b), br_ue(b), br_ue(b), br_ue(b), br_ue(b);
}

static void finish_picture(Hevc *d);

static void parse_sps(Hevc *d, Br *b) {
    HvSps *s = calloc(1, sizeof *s);
    if (!s) return;
    br_u(b, 4);
    int sub_layers = (int)br_u(b, 3);
    br_u(b, 1);
    if (sub_layers > 6) goto bad;
    ptl(b, sub_layers);
    int id = br_uer(b, 0, HV_MAX_SPS - 1);
    s->chroma = br_uer(b, 0, 3);
    if (s->chroma == 3) br_u(b, 1);
    s->width = br_uer(b, 8, 16888), s->height = br_uer(b, 8, 16888);
    if (br_u(b, 1)) {
        int cw = s->chroma == 1 || s->chroma == 2 ? 2 : 1, chh = s->chroma == 1 ? 2 : 1;
        s->crop_l = br_uer(b, 0, 8192) * cw, s->crop_r = br_uer(b, 0, 8192) * cw;
        s->crop_t = br_uer(b, 0, 8192) * chh, s->crop_b = br_uer(b, 0, 8192) * chh;
    }
    s->depth = br_uer(b, 0, 8) + 8, s->depth_c = br_uer(b, 0, 8) + 8;
    s->log2_poc_lsb = br_uer(b, 0, 12) + 4;
    bool ordering = br_u(b, 1);
    for (int i = ordering ? 0 : sub_layers; i <= sub_layers; i++) {
        s->max_dec = br_uer(b, 0, 15) + 1;
        s->max_reorder = br_uer(b, 0, 15);
        s->max_latency = br_uer(b, 0, 0x7fffffff);
    }
    s->log2_min_cb = br_uer(b, 0, 3) + 3;
    s->log2_ctb = s->log2_min_cb + br_uer(b, 0, 3);
    s->log2_min_tb = br_uer(b, 0, 3) + 2;
    s->log2_max_tb = s->log2_min_tb + br_uer(b, 0, 3);
    s->depth_inter = br_uer(b, 0, 4), s->depth_intra = br_uer(b, 0, 4);
    s->scaling = br_u(b, 1);
    if (s->scaling) {
        scaling_default(&s->sl);
        if (br_u(b, 1)) scaling_data(b, &s->sl);
    }
    s->amp = br_u(b, 1), s->sao = br_u(b, 1), s->pcm = br_u(b, 1);
    if (s->pcm) {
        s->pcm_depth = (int)br_u(b, 4) + 1, s->pcm_depth_c = (int)br_u(b, 4) + 1;
        s->log2_min_pcm = br_uer(b, 0, 2) + 3;
        s->log2_max_pcm = s->log2_min_pcm + br_uer(b, 0, 2);
        s->pcm_nofilter = br_u(b, 1);
    }
    s->nrps = br_uer(b, 0, HV_MAX_RPS);
    for (int i = 0; i < s->nrps && !b->err; i++)
        if (!st_rps(b, &s->rps[i], s->rps, i, s->nrps, false)) goto bad;
    s->lt_present = br_u(b, 1);
    if (s->lt_present) {
        s->nlt_sps = br_uer(b, 0, HV_MAX_LT);
        for (int i = 0; i < s->nlt_sps; i++)
            s->lt_lsb_sps[i] = (int)br_u(b, s->log2_poc_lsb), s->lt_used_sps[i] = (uint8_t)br_u(b, 1);
    }
    s->tmvp = br_u(b, 1), s->strong = br_u(b, 1);
    s->sar_num = s->sar_den = 1;
    if (br_u(b, 1)) vui(b, s, sub_layers);
    if (br_u(b, 1)) { /* sps_extension_present_flag */
        bool range = br_u(b, 1), multilayer = br_u(b, 1), ext3d = br_u(b, 1), scc = br_u(b, 1);
        br_u(b, 4);
        if (range && br_u(b, 9)) s->unsup = "the range extensions";
        if (multilayer || ext3d || scc) s->unsup = s->unsup ? s->unsup : "the multilayer or screen-content extensions";
    }
    if (b->err) goto bad;
    if (s->chroma != 1) s->unsup = s->chroma == 0 ? "monochrome pictures" : s->chroma == 2 ? "4:2:2 chroma" : "4:4:4 chroma";
    else if (s->depth > 10 || s->depth_c > 10) s->unsup = "more than 10 bits a sample";
    else if (s->depth != s->depth_c) s->unsup = "different luma and chroma bit depths";
    if (s->log2_ctb < 4 || s->log2_ctb > 6 || s->log2_max_tb > 5 || s->log2_max_tb > s->log2_ctb ||
        s->log2_min_tb >= s->log2_min_cb || (s->width & ((1 << s->log2_min_cb) - 1)) ||
        (s->height & ((1 << s->log2_min_cb) - 1)) || (int64_t)s->width * s->height > HV_MAX_PIXELS ||
        s->crop_l + s->crop_r >= s->width || s->crop_t + s->crop_b >= s->height ||
        (s->pcm && (s->log2_max_pcm > 5 || s->log2_max_pcm > s->log2_ctb || s->pcm_depth > s->depth ||
                    s->pcm_depth_c > s->depth_c)))
        goto bad;
    s->valid = true;
    if (d->sps[id] && !memcmp(d->sps[id], s, sizeof *s)) { /* the same again (every key frame brings it) */
        free(s);
        return;
    }
    if (d->s == d->sps[id]) { /* (the picture under way is done: its sets change) */
        if (d->cur) finish_picture(d);
        d->s = NULL, d->p = NULL;
    }
    free(d->sps[id]);
    d->sps[id] = s;
    return;
bad:
    free(s);
}

static void parse_pps(Hevc *d, Br *b) {
    HvPps *p = calloc(1, sizeof *p);
    if (!p) return;
    int id = br_uer(b, 0, HV_MAX_PPS - 1);
    p->sps = br_uer(b, 0, HV_MAX_SPS - 1);
    p->dep_slices = br_u(b, 1), p->output_flag = br_u(b, 1);
    p->extra_bits = (int)br_u(b, 3);
    p->sign_hiding = br_u(b, 1), p->cabac_init_present = br_u(b, 1);
    p->nref[0] = br_uer(b, 0, 14) + 1, p->nref[1] = br_uer(b, 0, 14) + 1;
    p->init_qp = 26 + br_ser(b, -(26 + 6 * 8), 25);
    p->constrained_intra = br_u(b, 1), p->transform_skip = br_u(b, 1);
    p->cu_qp_delta = br_u(b, 1);
    if (p->cu_qp_delta) p->diff_qp_depth = br_uer(b, 0, 3);
    p->cb_off = br_ser(b, -12, 12), p->cr_off = br_ser(b, -12, 12);
    p->slice_chroma_offsets = br_u(b, 1);
    p->weighted_p = br_u(b, 1), p->weighted_b = br_u(b, 1);
    p->bypass = br_u(b, 1);
    p->tiles = br_u(b, 1), p->wpp = br_u(b, 1);
    p->ncols = p->nrows = 1;
    p->lf_tiles = true;
    if (p->tiles) {
        p->ncols = br_uer(b, 0, HV_MAX_TILE - 1) + 1, p->nrows = br_uer(b, 0, HV_MAX_TILE - 1) + 1;
        p->uniform = br_u(b, 1);
        if (!p->uniform) {
            for (int i = 0; i < p->ncols - 1; i++) p->col_w[i] = br_uer(b, 0, 4095) + 1;
            for (int i = 0; i < p->nrows - 1; i++) p->row_h[i] = br_uer(b, 0, 4095) + 1;
        }
        p->lf_tiles = br_u(b, 1);
    }
    p->lf_slices = br_u(b, 1);
    if (br_u(b, 1)) {
        p->deblock_override = br_u(b, 1);
        p->deblock_off = br_u(b, 1);
        if (!p->deblock_off) p->beta = br_ser(b, -6, 6) * 2, p->tc = br_ser(b, -6, 6) * 2;
    }
    p->scaling = br_u(b, 1);
    if (p->scaling) {
        scaling_default(&p->sl);
        scaling_data(b, &p->sl);
    }
    p->lists_mod = br_u(b, 1);
    p->log2_merge = br_uer(b, 0, 4) + 2;
    p->hdr_ext = br_u(b, 1);
    if (br_u(b, 1)) { /* pps_extension_present_flag */
        bool range = br_u(b, 1), more = br_u(b, 1) | br_u(b, 1) | br_u(b, 1);
        br_u(b, 4);
        if (range || more) p->unsup = "the range or screen-content extensions";
    }
    if (b->err) {
        free(p);
        return;
    }
    p->valid = true;
    if (d->pps[id] && !memcmp(d->pps[id], p, sizeof *p)) {
        free(p);
        return;
    }
    if (d->p == d->pps[id]) {
        if (d->cur) finish_picture(d);
        d->p = NULL;
    }
    free(d->pps[id]);
    d->pps[id] = p;
}

/* ---- the active parameter sets: per-picture tables ---- */

static void *grow(void *old, size_t n) {
    free(old);
    return calloc(n ? n : 1, 1);
}

static void pic_free(HvPic *p) {
    for (int i = 0; i < 3; i++) free(p->base[i]);
    free(p->mv), free(p->ctb_slice), free(p->refs);
    memset(p, 0, sizeof *p);
}

/* the sequence's sizes changed: every picture, and the per-picture metadata, made again */
static bool sps_tables(Hevc *d, const HvSps *s) {
    d->ctb = 1 << s->log2_ctb;
    d->wctb = (s->width + d->ctb - 1) >> s->log2_ctb, d->hctb = (s->height + d->ctb - 1) >> s->log2_ctb;
    d->nctb = d->wctb * d->hctb;
    d->wmin = s->width >> s->log2_min_cb, d->hmin = s->height >> s->log2_min_cb;
    d->w4 = s->width >> 2, d->h4 = s->height >> 2;
    d->zw = (d->wctb << s->log2_ctb) >> s->log2_min_tb;
    int zh = (d->hctb << s->log2_ctb) >> s->log2_min_tb;
    size_t n4 = (size_t)d->w4 * d->h4, nc = (size_t)d->nctb;
    d->rs2ts = grow(d->rs2ts, nc * sizeof(int)), d->ts2rs = grow(d->ts2rs, (nc + 1) * sizeof(int));
    d->tile_id = grow(d->tile_id, (nc + 1) * sizeof(int));
    d->col_of = grow(d->col_of, nc * sizeof(int)), d->row_of = grow(d->row_of, nc * sizeof(int));
    d->zs = grow(d->zs, (size_t)d->zw * zh * sizeof(int));
    d->ctdepth = grow(d->ctdepth, (size_t)d->wmin * d->hmin), d->skipf = grow(d->skipf, (size_t)d->wmin * d->hmin);
    d->ipm = grow(d->ipm, n4), d->flags4 = grow(d->flags4, n4), d->qp4 = grow(d->qp4, n4);
    d->bs_v = grow(d->bs_v, (size_t)((s->width + 7) >> 3) * d->h4);
    d->bs_h = grow(d->bs_h, (size_t)d->w4 * ((s->height + 7) >> 3));
    d->ctb_addr = grow(d->ctb_addr, nc * sizeof(int16_t)), d->ctb_info = grow(d->ctb_info, nc * sizeof(int16_t));
    d->sao = grow(d->sao, nc * sizeof(HvSao));
    size_t ntmp = (size_t)(s->width + 2) * (s->height + 2);
    d->tmp = grow(d->tmp, ntmp * sizeof(px)), d->ntmp = ntmp;
    if (!d->rs2ts || !d->ts2rs || !d->tile_id || !d->col_of || !d->row_of || !d->zs || !d->ctdepth || !d->skipf ||
        !d->ipm || !d->flags4 || !d->qp4 || !d->bs_v || !d->bs_h || !d->ctb_addr || !d->ctb_info || !d->sao ||
        !d->tmp)
        return false;
    for (int i = 0; i < HV_MAX_DPB; i++)
        if (!d->pics[i].busy) pic_free(&d->pics[i]);
    return true;
}

static void pps_tables(Hevc *d, const HvSps *s, const HvPps *p) {
    int ncols = CLIP3(1, d->wctb, p->ncols), nrows = CLIP3(1, d->hctb, p->nrows);
    int cw[HV_MAX_TILE], rh[HV_MAX_TILE];
    for (int i = 0; i < ncols; i++)
        cw[i] = p->uniform || !p->tiles ? ((i + 1) * d->wctb) / ncols - (i * d->wctb) / ncols : p->col_w[i];
    for (int j = 0; j < nrows; j++)
        rh[j] = p->uniform || !p->tiles ? ((j + 1) * d->hctb) / nrows - (j * d->hctb) / nrows : p->row_h[j];
    if (p->tiles && !p->uniform) { /* the last column / row takes what's left */
        int sum = 0;
        for (int i = 0; i < ncols - 1; i++) sum += cw[i];
        cw[ncols - 1] = d->wctb - sum;
        sum = 0;
        for (int j = 0; j < nrows - 1; j++) sum += rh[j];
        rh[nrows - 1] = d->hctb - sum;
    }
    d->col_bd[0] = d->row_bd[0] = 0;
    for (int i = 0; i < ncols; i++) d->col_bd[i + 1] = d->col_bd[i] + CLIP3(0, d->wctb, cw[i]);
    for (int j = 0; j < nrows; j++) d->row_bd[j + 1] = d->row_bd[j] + CLIP3(0, d->hctb, rh[j]);
    for (int rs = 0; rs < d->nctb; rs++) {
        int tx = rs % d->wctb, ty = rs / d->wctb, ti = 0, tj = 0;
        for (int i = 0; i < ncols; i++)
            if (tx >= d->col_bd[i]) ti = i;
        for (int j = 0; j < nrows; j++)
            if (ty >= d->row_bd[j]) tj = j;
        int ts = 0;
        for (int i = 0; i < ti; i++) ts += rh[tj] * cw[i];
        for (int j = 0; j < tj; j++) ts += d->wctb * rh[j];
        ts += (ty - d->row_bd[tj]) * cw[ti] + tx - d->col_bd[ti];
        if (ts < 0 || ts >= d->nctb) ts = rs; /* (column widths that don't add up: a broken PPS) */
        d->rs2ts[rs] = ts;
        d->col_of[rs] = ti, d->row_of[rs] = tj;
    }
    for (int rs = 0; rs < d->nctb; rs++) d->ts2rs[d->rs2ts[rs]] = rs;
    for (int rs = 0; rs < d->nctb; rs++) d->tile_id[d->rs2ts[rs]] = d->row_of[rs] * ncols + d->col_of[rs];
    d->ts2rs[d->nctb] = d->nctb, d->tile_id[d->nctb] = -1;
    int shift = s->log2_ctb - s->log2_min_tb, zh = (d->hctb << s->log2_ctb) >> s->log2_min_tb;
    for (int y = 0; y < zh; y++)
        for (int x = 0; x < d->zw; x++) {
            int rs = d->wctb * ((y << s->log2_min_tb) >> s->log2_ctb) + ((x << s->log2_min_tb) >> s->log2_ctb);
            int v = d->rs2ts[rs] << (shift * 2);
            for (int i = 0; i < shift; i++) {
                int m = 1 << i;
                v += (m & x ? m * m : 0) + (m & y ? 2 * m * m : 0);
            }
            d->zs[y * d->zw + x] = v;
        }
    /* scaling factors m[x][y]: the PPS's lists, else the SPS's, else flat 16 */
    const HvScaling *sl = !s->scaling ? NULL : p->scaling ? &p->sl : &s->sl;
    d->scale_flat = !sl;
    static const uint8_t D4[16][2] = {{0, 0}, {0, 1}, {1, 0}, {0, 2}, {1, 1}, {2, 0}, {0, 3}, {1, 2},
                                      {2, 1}, {3, 0}, {1, 3}, {2, 2}, {3, 1}, {2, 3}, {3, 2}, {3, 3}};
    uint8_t d8[64][2];
    { /* the 8x8 up-right diagonal scan */
        int i = 0, x = 0, y = 0;
        while (i < 64) {
            while (y >= 0) {
                if (x < 8 && y < 8) d8[i][0] = (uint8_t)x, d8[i][1] = (uint8_t)y, i++;
                y--, x++;
            }
            y = x, x = 0;
        }
    }
    if (sl)
        for (int m = 0; m < 6; m++) {
            for (int i = 0; i < 16; i++) d->scale[0][m][D4[i][1] * 4 + D4[i][0]] = sl->list[0][m][i];
            for (int sz = 1; sz < 4; sz++) {
                int n = 4 << sz, rep = n / 8;
                for (int i = 0; i < 64; i++)
                    for (int ky = 0; ky < rep; ky++)
                        for (int kx = 0; kx < rep; kx++)
                            d->scale[sz][m][(d8[i][1] * rep + ky) * n + d8[i][0] * rep + kx] = sl->list[sz][m][i];
                if (sz > 1) d->scale[sz][m][0] = sl->dc[sz][m];
            }
        }
}

/* ---- pictures ---- */

static HvPic *pic_new(Hevc *d) {
    const HvSps *s = d->s;
    HvPic *p = NULL;
    for (int i = 0; i < HV_MAX_DPB && !p; i++)
        if (!d->pics[i].busy) p = &d->pics[i];
    if (!p) return NULL;
    int cw = s->width >> 1, ch = s->height >> 1;
    if (!p->base[0] || p->w != s->width || p->h != s->height || p->nctb != d->nctb) {
        pic_free(p);
        p->w = s->width, p->h = s->height, p->cw = cw, p->ch = ch, p->nctb = d->nctb;
        for (int c = 0; c < 3; c++) {
            int pad = c ? HV_PAD / 2 : HV_PAD, w = c ? cw : s->width, h = c ? ch : s->height;
            p->stride[c] = w + 2 * pad;
            p->base[c] = calloc((size_t)p->stride[c] * (h + 2 * pad), sizeof(px));
            if (!p->base[c]) return pic_free(p), NULL;
            p->plane[c] = p->base[c] + (size_t)pad * p->stride[c] + pad;
        }
        p->mv = calloc((size_t)d->w4 * d->h4, sizeof(HvMv));
        p->ctb_slice = malloc((size_t)d->nctb * sizeof(int16_t));
        if (!p->mv || !p->ctb_slice) return pic_free(p), NULL;
    }
    p->busy = true;
    p->ref = 0, p->output = p->handed = p->missing = false;
    p->nrefs = 0, p->damaged = 0, p->pts = HEVC_NOPTS, p->seq = d->seq;
    p->crop_l = s->crop_l, p->crop_r = s->crop_r, p->crop_t = s->crop_t, p->crop_b = s->crop_b;
    p->depth = s->depth, p->sar_num = s->sar_num, p->sar_den = s->sar_den;
    p->rate_num = s->rate_num, p->rate_den = s->rate_den;
    for (int i = 0; i < d->nctb; i++) p->ctb_slice[i] = -1;
    return p;
}

static void pic_release(Hevc *d, HvPic *p) {
    (void)d;
    if (p->ref || p->output || p->handed || p == d->cur) return;
    p->busy = false;
}

static void pad_picture(HvPic *p) { /* the edges repeated outward: motion may point outside the picture */
    for (int c = 0; c < 3; c++) {
        int pad = c ? HV_PAD / 2 : HV_PAD, w = c ? p->cw : p->w, h = c ? p->ch : p->h, st = p->stride[c];
        px *a = p->plane[c];
        for (int y = 0; y < h; y++) {
            px *row = a + (size_t)y * st;
            for (int x = 1; x <= pad; x++) row[-x] = row[0], row[w - 1 + x] = row[w - 1];
        }
        for (int y = 1; y <= pad; y++) {
            memcpy(a - pad - (size_t)y * st, a - pad, (size_t)st * sizeof(px));
            memcpy(a - pad + (size_t)(h - 1 + y) * st, a - pad + (size_t)(h - 1) * st, (size_t)st * sizeof(px));
        }
    }
}

static void grey(HvPic *p, int depth) {
    px v = (px)(1 << (depth - 1));
    for (int c = 0; c < 3; c++) {
        int w = c ? p->cw : p->w, h = c ? p->ch : p->h;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) p->plane[c][(size_t)y * p->stride[c] + x] = v;
    }
}

static void out_push(Hevc *d, HvPic *p) {
    p->output = false;
    if (d->nout < HV_MAX_DPB) d->out_fifo[d->nout++] = p, p->handed = true;
    else pic_release(d, p);
}

/* output the waiting picture of the smallest POC (an earlier sequence's first) */
static bool bump(Hevc *d) {
    HvPic *best = NULL;
    for (int i = 0; i < HV_MAX_DPB; i++) {
        HvPic *p = &d->pics[i];
        if (!p->busy || !p->output || p == d->cur) continue;
        if (!best || p->seq < best->seq || (p->seq == best->seq && p->poc < best->poc)) best = p;
    }
    if (!best) return false;
    out_push(d, best);
    return true;
}

static int waiting(const Hevc *d) {
    int n = 0;
    for (int i = 0; i < HV_MAX_DPB; i++)
        if (d->pics[i].busy && d->pics[i].output && &d->pics[i] != d->cur) n++;
    return n;
}

static int held(const Hevc *d) {
    int n = 0;
    for (int i = 0; i < HV_MAX_DPB; i++)
        if (d->pics[i].busy && (d->pics[i].output || d->pics[i].ref) && &d->pics[i] != d->cur) n++;
    return n;
}

static void finish_picture(Hevc *d) {
    HvPic *p = d->cur;
    if (!p) return;
    d->cur = NULL;
    if (!d->s || !d->p) { /* (its parameter sets are gone: it can't be finished - dropped) */
        p->output = false, p->ref = 0;
        pic_release(d, p);
        return;
    }
    d->cur = p;
    hv_deblock(d);
    hv_sao(d);
    pad_picture(p);
    d->cur = NULL;
    while (waiting(d) > d->s->max_reorder && bump(d)) {
    }
    pic_release(d, p);
}

/* ---- reference picture sets ---- */

static HvPic *find_poc(Hevc *d, int poc, int mask, bool lt_only_lsb, int want_ref) {
    for (int i = 0; i < HV_MAX_DPB; i++) {
        HvPic *p = &d->pics[i];
        if (!p->busy || !p->ref || p == d->cur || p->seq != d->seq) continue;
        if (want_ref && p->ref != want_ref) continue;
        if ((lt_only_lsb ? (p->poc & mask) : p->poc) == poc) return p;
    }
    return NULL;
}

static HvPic *make_missing(Hevc *d, int poc, int ref) {
    HvPic *p = pic_new(d);
    if (!p) return NULL;
    grey(p, d->s->depth);
    memset(p->mv, 0, (size_t)d->w4 * d->h4 * sizeof(HvMv));
    pad_picture(p);
    p->poc = poc, p->ref = ref, p->missing = true;
    return p;
}

/* 8.3.2: mark the DPB's pictures by this picture's reference set; ones that aren't there are made up (grey) */
static void apply_rps(Hevc *d, int *nb, int *na, int *nl, HvPic **bl, HvPic **al, HvPic **ll) {
    const HvSps *s = d->s;
    const HvSlice *sh = &d->sh;
    int irap = d->nal_type >= NAL_BLA_W_LP && d->nal_type <= 23;
    *nb = *na = *nl = 0;
    bool keep[HV_MAX_DPB] = {0}, lt[HV_MAX_DPB] = {0};
    if (irap && d->no_rasl) {
        for (int i = 0; i < HV_MAX_DPB; i++) d->pics[i].ref = 0;
        return;
    }
    int maxlsb = 1 << s->log2_poc_lsb;
    const HvRps *r = sh->rps;
    int st_poc[HV_MAX_REF], st_kind[HV_MAX_REF], nst = 0; /* kind: 0 before, 1 after, 2 foll */
    if (r)
        for (int i = 0; i < r->nneg + r->npos; i++)
            st_poc[nst] = d->poc + r->delta[i], st_kind[nst++] = r->used[i] ? (i < r->nneg ? 0 : 1) : 2;
    /* long-term first */
    for (int i = 0; i < sh->nlt_sps + sh->nlt; i++) {
        int poc = sh->lt_lsb[i];
        if (sh->lt_msb[i]) poc += d->poc - sh->lt_cycle[i] * maxlsb - (d->poc & (maxlsb - 1));
        HvPic *p = find_poc(d, poc, maxlsb - 1, !sh->lt_msb[i], 0);
        if (p) keep[p - d->pics] = true, lt[p - d->pics] = true;
        if (sh->lt_used[i] && *nl < HV_MAX_LT) {
            if (!p && (p = make_missing(d, poc, 2))) keep[p - d->pics] = true, lt[p - d->pics] = true;
            ll[(*nl)++] = p;
        }
    }
    for (int i = 0; i < nst; i++) {
        HvPic *p = NULL;
        for (int k = 0; k < HV_MAX_DPB && !p; k++) {
            HvPic *q = &d->pics[k];
            if (q->busy && q->ref == 1 && !lt[k] && q != d->cur && q->seq == d->seq && q->poc == st_poc[i]) p = q;
        }
        if (p) keep[p - d->pics] = true;
        if (st_kind[i] < 2) {
            if (!p && (p = make_missing(d, st_poc[i], 1))) keep[p - d->pics] = true;
            if (st_kind[i] == 0) bl[(*nb)++] = p;
            else al[(*na)++] = p;
        }
    }
    for (int i = 0; i < HV_MAX_DPB; i++) {
        HvPic *p = &d->pics[i];
        if (p == d->cur || !p->busy) continue;
        if (!keep[i]) p->ref = 0;
        else p->ref = lt[i] ? 2 : 1;
        pic_release(d, p);
    }
}

/* ---- slice headers ---- */

static bool slice_header(Hevc *d, Br *b, HvSlice *sh) {
    int nt = d->nal_type;
    sh->first = (int)br_u(b, 1);
    if (nt >= NAL_BLA_W_LP && nt <= 23) sh->no_output_prior = (int)br_u(b, 1);
    sh->pps = br_uer(b, 0, HV_MAX_PPS - 1);
    const HvPps *p = d->pps[sh->pps];
    if (b->err || !p || !p->valid || !d->sps[p->sps] || !d->sps[p->sps]->valid) return false;
    const HvSps *s = d->sps[p->sps];
    sh->dependent = 0, sh->address = 0;
    if (!sh->first) {
        if (p->dep_slices) sh->dependent = (int)br_u(b, 1);
        int wctb = (s->width + (1 << s->log2_ctb) - 1) >> s->log2_ctb;
        int hctb = (s->height + (1 << s->log2_ctb) - 1) >> s->log2_ctb;
        sh->address = (int)br_u(b, ceil_log2(wctb * hctb));
        if (sh->address >= wctb * hctb) return false;
    }
    if (sh->dependent) goto entry; /* everything else as the slice before it */
    br_u(b, p->extra_bits);
    sh->type = br_uer(b, 0, 2);
    sh->output = p->output_flag ? (int)br_u(b, 1) : 1;
    sh->poc_lsb = 0;
    sh->rps = NULL, sh->nlt_sps = sh->nlt = 0, sh->tmvp = false;
    if (nt != NAL_IDR_W_RADL && nt != NAL_IDR_N_LP) {
        sh->poc_lsb = (int)br_u(b, s->log2_poc_lsb);
        if (!br_u(b, 1)) {
            if (!st_rps(b, &sh->rps_own, s->rps, s->nrps, s->nrps, true)) return false;
            sh->rps = &sh->rps_own;
        } else {
            if (!s->nrps) return false;
            int idx = s->nrps > 1 ? (int)br_u(b, ceil_log2(s->nrps)) : 0;
            if (idx >= s->nrps) return false;
            sh->rps = &s->rps[idx];
        }
        if (s->lt_present) {
            if (s->nlt_sps) sh->nlt_sps = br_uer(b, 0, s->nlt_sps);
            sh->nlt = br_uer(b, 0, HV_MAX_LT - sh->nlt_sps);
            for (int i = 0; i < sh->nlt_sps + sh->nlt && !b->err; i++) {
                if (i < sh->nlt_sps) {
                    int k = s->nlt_sps > 1 ? (int)br_u(b, ceil_log2(s->nlt_sps)) : 0;
                    if (k >= s->nlt_sps) return false;
                    sh->lt_lsb[i] = s->lt_lsb_sps[k], sh->lt_used[i] = s->lt_used_sps[k];
                } else {
                    sh->lt_lsb[i] = (int)br_u(b, s->log2_poc_lsb), sh->lt_used[i] = (uint8_t)br_u(b, 1);
                }
                sh->lt_msb[i] = (uint8_t)br_u(b, 1);
                int cycle = sh->lt_msb[i] ? br_uer(b, 0, 1 << 24) : 0;
                sh->lt_cycle[i] = (i == 0 || i == sh->nlt_sps) ? cycle : cycle + sh->lt_cycle[i - 1];
                /* a picture more than 2^30 POCs back: no POC (an int) can name it - a damaged header (7.4.7.1) */
                if ((int64_t)sh->lt_cycle[i] << s->log2_poc_lsb > (1 << 30)) return false;
            }
        }
        if (s->tmvp) sh->tmvp = br_u(b, 1);
    }
    sh->sao_luma = sh->sao_chroma = false;
    if (s->sao) sh->sao_luma = br_u(b, 1), sh->sao_chroma = br_u(b, 1);
    sh->nref[0] = sh->nref[1] = 0;
    sh->mod[0] = sh->mod[1] = false;
    sh->mvd_l1_zero = sh->cabac_init = false, sh->col_l0 = true, sh->col_ref = 0;
    memset(sh->wflag, 0, sizeof sh->wflag);
    if (sh->type != SLICE_I) {
        sh->nref[0] = p->nref[0], sh->nref[1] = sh->type == SLICE_B ? p->nref[1] : 0;
        if (br_u(b, 1)) {
            sh->nref[0] = br_uer(b, 0, 14) + 1;
            if (sh->type == SLICE_B) sh->nref[1] = br_uer(b, 0, 14) + 1;
        }
        int total = 0;
        if (sh->rps)
            for (int i = 0; i < sh->rps->nneg + sh->rps->npos; i++) total += sh->rps->used[i];
        for (int i = 0; i < sh->nlt_sps + sh->nlt; i++) total += sh->lt_used[i];
        if (p->lists_mod && total > 1)
            for (int l = 0; l < (sh->type == SLICE_B ? 2 : 1); l++) {
                sh->mod[l] = br_u(b, 1);
                if (sh->mod[l])
                    for (int i = 0; i < sh->nref[l]; i++) sh->entry[l][i] = (int)br_u(b, ceil_log2(total));
            }
        if (sh->type == SLICE_B) sh->mvd_l1_zero = br_u(b, 1);
        if (p->cabac_init_present) sh->cabac_init = br_u(b, 1);
        if (sh->tmvp) {
            if (sh->type == SLICE_B) sh->col_l0 = br_u(b, 1);
            if (sh->nref[sh->col_l0 ? 0 : 1] > 1) sh->col_ref = br_uer(b, 0, sh->nref[sh->col_l0 ? 0 : 1] - 1);
        }
        if ((p->weighted_p && sh->type == SLICE_P) || (p->weighted_b && sh->type == SLICE_B)) {
            sh->lw_denom = br_uer(b, 0, 7);
            sh->cw_denom = sh->lw_denom + br_ser(b, -7, 7);
            if (sh->cw_denom < 0 || sh->cw_denom > 7) return false;
            for (int l = 0; l < (sh->type == SLICE_B ? 2 : 1); l++) {
                bool lf[HV_MAX_REF], cf[HV_MAX_REF];
                for (int i = 0; i < sh->nref[l]; i++) lf[i] = br_u(b, 1);
                for (int i = 0; i < sh->nref[l]; i++) cf[i] = br_u(b, 1);
                for (int i = 0; i < sh->nref[l]; i++) {
                    sh->lw[l][i] = (int16_t)(1 << sh->lw_denom), sh->lo[l][i] = 0;
                    if (lf[i]) sh->lw[l][i] = (int16_t)(sh->lw[l][i] + br_ser(b, -128, 127)), sh->lo[l][i] = (int16_t)br_ser(b, -128, 127);
                    for (int c = 0; c < 2; c++) {
                        sh->cw[l][i][c] = (int16_t)(1 << sh->cw_denom), sh->co[l][i][c] = 0;
                        if (cf[i]) {
                            int w = (1 << sh->cw_denom) + br_ser(b, -128, 127), dof = br_ser(b, -512, 511);
                            sh->cw[l][i][c] = (int16_t)w;
                            sh->co[l][i][c] = (int16_t)CLIP3(-128, 127, (128 - ((128 * w) >> sh->cw_denom) + dof));
                        }
                    }
                    sh->wflag[l][i] = true;
                }
            }
        }
        sh->max_merge = 5 - br_uer(b, 0, 4);
    }
    sh->qp = p->init_qp + br_se(b);
    if (sh->qp < -6 * (s->depth - 8) || sh->qp > 51) return false;
    sh->cb_off = sh->cr_off = 0;
    if (p->slice_chroma_offsets) sh->cb_off = br_ser(b, -12, 12), sh->cr_off = br_ser(b, -12, 12);
    bool override = p->deblock_override ? br_u(b, 1) : false;
    sh->deblock_off = p->deblock_off, sh->beta = p->beta, sh->tc = p->tc;
    if (override) {
        sh->deblock_off = br_u(b, 1);
        if (!sh->deblock_off) sh->beta = br_ser(b, -6, 6) * 2, sh->tc = br_ser(b, -6, 6) * 2;
    }
    sh->lf_slices = p->lf_slices;
    if (p->lf_slices && (sh->sao_luma || sh->sao_chroma || !sh->deblock_off)) sh->lf_slices = br_u(b, 1);
entry:
    if (p->tiles || p->wpp) {
        int n = br_uer(b, 0, 1 << 16);
        if (n) { /* entry points: not needed - each substream starts where the one before ended */
            int len = br_uer(b, 0, 31) + 1;
            for (int i = 0; i < n && !b->err; i++) br_u(b, len);
        }
    }
    if (p->hdr_ext) {
        int n = br_uer(b, 0, 256);
        for (int i = 0; i < n; i++) br_u(b, 8);
    }
    if (b->err || !br_u(b, 1)) return false; /* byte_alignment(): a one, then zeros */
    while (b->pos & 7) br_u(b, 1);
    sh->data = b->pos >> 3;
    return !b->err;
}

/* ---- a picture starts ---- */

static bool activate(Hevc *d, int pps_id) {
    const HvPps *p = d->pps[pps_id];
    const HvSps *s = d->sps[p->sps];
    if (d->s != s) {
        if (s->unsup) {
            d->unsup = s->unsup;
            return false;
        }
        if (!d->s || d->s->width != s->width || d->s->height != s->height || d->s->log2_ctb != s->log2_ctb ||
            d->s->log2_min_cb != s->log2_min_cb || d->s->log2_min_tb != s->log2_min_tb) {
            while (bump(d)) { /* (what's waiting comes out first: pictures of the new size are made) */
            }
            for (int i = 0; i < HV_MAX_DPB; i++) d->pics[i].ref = 0, pic_release(d, &d->pics[i]);
            d->s = s;
            if (!sps_tables(d, s)) {
                d->s = NULL;
                return false;
            }
        }
        d->s = s;
        d->p = NULL;
    }
    if (p->unsup) {
        d->unsup = p->unsup;
        return false;
    }
    if (d->p != p) {
        d->p = p;
        pps_tables(d, s, p);
    }
    d->unsup = NULL;
    return true;
}

static bool start_picture(Hevc *d) {
    const HvSps *s = d->s;
    HvSlice *sh = &d->sh;
    int nt = d->nal_type;
    bool irap = nt >= NAL_BLA_W_LP && nt <= 23;
    if (irap) {
        d->no_rasl = nt != NAL_CRA || d->first_pic || d->eos || d->wait_irap;
        d->wait_irap = false;
    } else if (d->wait_irap) {
        return false;
    }
    if ((nt == NAL_RASL_N || nt == NAL_RASL_R) && d->no_rasl) return false; /* its references were never sent */
    if (irap && d->no_rasl) {
        while (bump(d)) {
        }
        d->seq++;
    }
    int maxlsb = 1 << s->log2_poc_lsb, msb;
    if (irap && d->no_rasl) msb = 0;
    else {
        int prev_lsb = d->prev_tid0_poc & (maxlsb - 1), prev_msb = d->prev_tid0_poc - prev_lsb;
        if (sh->poc_lsb < prev_lsb && prev_lsb - sh->poc_lsb >= maxlsb / 2) msb = prev_msb + maxlsb;
        else if (sh->poc_lsb > prev_lsb && sh->poc_lsb - prev_lsb > maxlsb / 2) msb = prev_msb - maxlsb;
        else msb = prev_msb;
    }
    d->poc = msb + sh->poc_lsb;
    bool slnr = nt <= 14 && !(nt & 1);
    if (d->temporal_id == 0 && !(nt >= NAL_RADL_N && nt <= NAL_RASL_R) && !slnr) d->prev_tid0_poc = d->poc;
    d->first_pic = d->eos = false;
    /* the references; then room for the new picture */
    int nb, na, nl;
    HvPic *bl[HV_MAX_REF], *al[HV_MAX_REF], *ll[HV_MAX_LT];
    apply_rps(d, &nb, &na, &nl, bl, al, ll);
    while ((waiting(d) > s->max_reorder || held(d) >= s->max_dec) && bump(d)) {
    }
    HvPic *p = pic_new(d);
    if (!p) { /* every picture held: let go of the oldest waiting one */
        bump(d);
        if (!(p = pic_new(d))) return false;
    }
    d->cur = p;
    p->poc = d->poc, p->ref = 1, p->output = sh->output != 0, p->pts = d->pts, p->type = "BPI"[sh->type];
    memset(p->mv, 0, (size_t)d->w4 * d->h4 * sizeof(HvMv));
    d->pts = HEVC_NOPTS; /* (a second picture in the same block has no time of its own) */
    for (int i = 0; i < d->nctb; i++) d->ctb_addr[i] = -1, d->ctb_info[i] = 0;
    memset(d->bs_v, 0, (size_t)((s->width + 7) >> 3) * d->h4);
    memset(d->bs_h, 0, (size_t)d->w4 * ((s->height + 7) >> 3));
    memset(d->flags4, 0, (size_t)d->w4 * d->h4);
    memset(d->sao, 0, (size_t)d->nctb * sizeof(HvSao));
    d->ninfo = 0;
    grey(p, s->depth); /* (what no slice reaches stays grey) */
    (void)bl, (void)al, (void)ll;
    return true;
}

/* RefPicList0/1 for this slice (8.3.4), from the sets apply_rps found - looked up again by POC */
static bool ref_lists(Hevc *d) {
    HvSlice *sh = &d->sh;
    const HvSps *s = d->s;
    memset(d->refpic, 0, sizeof d->refpic);
    if (sh->type == SLICE_I) return true;
    HvPic *bl[HV_MAX_REF], *al[HV_MAX_REF], *ll[HV_MAX_LT];
    int nb = 0, na = 0, nl = 0, maxlsb = 1 << s->log2_poc_lsb;
    const HvRps *r = sh->rps;
    if (r)
        for (int i = 0; i < r->nneg + r->npos; i++) {
            if (!r->used[i]) continue;
            int poc = d->poc + r->delta[i];
            HvPic *p = find_poc(d, poc, 0, false, 1);
            if (i < r->nneg) bl[nb++] = p;
            else al[na++] = p;
        }
    for (int i = 0; i < sh->nlt_sps + sh->nlt; i++) {
        if (!sh->lt_used[i] || nl >= HV_MAX_LT) continue;
        int poc = sh->lt_lsb[i];
        if (sh->lt_msb[i]) poc += d->poc - sh->lt_cycle[i] * maxlsb - (d->poc & (maxlsb - 1));
        ll[nl++] = find_poc(d, poc, maxlsb - 1, !sh->lt_msb[i], 2);
    }
    int total = nb + na + nl;
    if (!total) return false;
    for (int l = 0; l < (sh->type == SLICE_B ? 2 : 1); l++) {
        HvPic *tmp[HV_MAX_REF * 2];
        int n = sh->nref[l] > total ? sh->nref[l] : total, k = 0;
        if (n > HV_MAX_REF * 2) n = HV_MAX_REF * 2;
        while (k < n) {
            for (int i = 0; i < (l ? na : nb) && k < n; i++) tmp[k++] = l ? al[i] : bl[i];
            for (int i = 0; i < (l ? nb : na) && k < n; i++) tmp[k++] = l ? bl[i] : al[i];
            for (int i = 0; i < nl && k < n; i++) tmp[k++] = ll[i];
        }
        for (int i = 0; i < sh->nref[l]; i++) {
            int e = sh->mod[l] ? sh->entry[l][i] : i;
            HvPic *p = e < n ? tmp[e] : NULL;
            if (!p) return false;
            d->refpic[l][i] = p;
        }
    }
    /* NoBackwardPredFlag: no reference after this picture */
    d->lowdelay = true;
    for (int l = 0; l < 2; l++)
        for (int i = 0; i < sh->nref[l]; i++)
            if (d->refpic[l][i]->poc > d->poc) d->lowdelay = false;
    return true;
}

static void decode_slice(Hevc *d, const uint8_t *rbsp, size_t n) {
    Br b = {rbsp, n, 16, false};
    HvSlice next = d->sh;
    if (!slice_header(d, &b, &next)) {
        if (d->cur) d->cur->damaged++;
        return;
    }
    if (next.rps == &next.rps_own) next.rps = &d->sh.rps_own; /* (where it's about to be copied to) */
    if (next.first) {
        if (d->cur) finish_picture(d);
        if (next.dependent) return;
        if (!activate(d, next.pps)) return;
        d->sh = next;
        if (!start_picture(d)) return;
        d->last_type = d->nal_type;
    } else {
        if (!d->cur || d->pps[next.pps] != d->p) return; /* (its picture's first slice never came) */
        if (next.dependent && !d->have_dep && !d->sh.first && d->slice_addr_rs < 0) return;
        if (!next.dependent) next.first = 0;
        d->sh = next;
    }
    HvSlice *sh = &d->sh;
    if (!sh->dependent) d->slice_addr_rs = sh->address;
    if (sh->type != SLICE_I && !ref_lists(d)) {
        d->cur->damaged++;
        return;
    }
    /* what the loop filters and later pictures' temporal vectors need of this slice */
    if (d->ninfo == d->cinfo) {
        int c = d->cinfo ? d->cinfo * 2 : 16;
        HvSliceInfo *ni = realloc(d->info, (size_t)c * sizeof *ni);
        if (!ni) return;
        d->info = ni, d->cinfo = c;
    }
    d->info[d->ninfo] = (HvSliceInfo){d->slice_addr_rs, sh->deblock_off, sh->lf_slices, sh->sao_luma,
                                       sh->sao_chroma, sh->beta, sh->tc};
    HvPic *p = d->cur;
    if (p->nrefs == p->crefs) {
        int c = p->crefs ? p->crefs * 2 : 8;
        HvRefs *nr = realloc(p->refs, (size_t)c * sizeof *nr);
        if (!nr) return;
        p->refs = nr, p->crefs = c;
    }
    HvRefs *rf = &p->refs[p->nrefs];
    memset(rf, 0, sizeof *rf);
    for (int l = 0; l < 2; l++) {
        rf->n[l] = sh->nref[l];
        for (int i = 0; i < sh->nref[l]; i++) rf->poc[l][i] = d->refpic[l][i]->poc, rf->lt[l][i] = d->refpic[l][i]->ref == 2;
    }
    d->slice_no = p->nrefs++;
    d->ninfo++;
    if (sh->data > n || !hv_slice_data(d, rbsp + sh->data, n - sh->data)) p->damaged++;
}

/* ---- NAL units ---- */

static size_t unescape(Hevc *d, const uint8_t *p, size_t n) {
    if (n > d->crbsp) {
        uint8_t *r = realloc(d->rbsp, n + 16);
        if (!r) return 0;
        d->rbsp = r, d->crbsp = n;
    }
    size_t o = 0;
    int zeros = 0;
    for (size_t i = 0; i < n; i++) {
        if (zeros >= 2 && p[i] == 3) {
            zeros = 0;
            continue;
        }
        zeros = p[i] ? 0 : zeros + 1;
        d->rbsp[o++] = p[i];
    }
    memset(d->rbsp + o, 0, 16);
    return o;
}

static void nal(Hevc *d, const uint8_t *p, size_t n) {
    if (n < 2 || (p[0] & 0x80)) return;
    int type = (p[0] >> 1) & 63, layer = ((p[0] & 1) << 5) | (p[1] >> 3), tid = (p[1] & 7) - 1;
    if (layer || tid < 0) return;
    size_t m = unescape(d, p, n);
    if (m < 2) return;
    Br b = {d->rbsp, m, 16, false};
    switch (type) {
    case NAL_SPS: parse_sps(d, &b); break;
    case NAL_PPS: parse_pps(d, &b); break;
    case NAL_EOS:
    case NAL_EOB:
        if (d->cur) finish_picture(d);
        d->eos = true;
        break;
    default:
        if (type <= NAL_RASL_R || (type >= NAL_BLA_W_LP && type <= NAL_CRA)) {
            d->nal_type = type, d->temporal_id = tid;
            decode_slice(d, d->rbsp, m);
        }
    }
}

/* ---- the API ---- */

Hevc *hevc_new(void) {
    Hevc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->first_pic = true;
    d->act_sps = d->act_pps = -1;
    d->slice_addr_rs = -1;
    d->pts = HEVC_NOPTS;
    return d;
}

void hevc_free(Hevc *d) {
    if (!d) return;
    for (int i = 0; i < HV_MAX_SPS; i++) free(d->sps[i]);
    for (int i = 0; i < HV_MAX_PPS; i++) free(d->pps[i]);
    for (int i = 0; i < HV_MAX_DPB; i++) pic_free(&d->pics[i]);
    free(d->rs2ts), free(d->ts2rs), free(d->tile_id), free(d->col_of), free(d->row_of), free(d->zs);
    free(d->ctdepth), free(d->skipf), free(d->ipm), free(d->flags4), free(d->qp4), free(d->bs_v), free(d->bs_h);
    free(d->ctb_addr), free(d->ctb_info), free(d->sao), free(d->info), free(d->tmp);
    free(d->out8), free(d->rbsp), free(d->annexb);
    free(d);
}

bool hevc_config(Hevc *d, const uint8_t *p, size_t n) {
    if (n < 23 || p[0] != 1) return false;
    int len = (p[21] & 3) + 1, narr = p[22];
    size_t o = 23;
    for (int a = 0; a < narr; a++) {
        if (o + 3 > n) return false;
        int cnt = (p[o + 1] << 8) | p[o + 2];
        o += 3;
        for (int k = 0; k < cnt; k++) {
            if (o + 2 > n) return false;
            size_t l = (size_t)((p[o] << 8) | p[o + 1]);
            o += 2;
            if (o + l > n) return false;
            nal(d, p + o, l);
            o += l;
        }
    }
    if (len == 3) return false;
    d->nal_len = len;
    d->au_mode = true;
    return true;
}

static size_t start_code(const uint8_t *b, size_t n, size_t i) {
    for (; i + 3 <= n; i++)
        if (!b[i] && !b[i + 1] && b[i + 2] == 1) return i;
    return (size_t)-1;
}

/* An Annex B stream is decoded as frames are taken: NAL units are read from the buffer only while no frame is
   waiting (hevc_frame asks for more) - a piece of input holding a hundred small pictures never holds them all. */
static void pump(Hevc *d) {
    while (!d->nout) {
        size_t s = start_code(d->annexb, d->nannexb, d->apos);
        if (s == (size_t)-1) {
            d->apos = d->nannexb > 3 ? d->nannexb - 3 : 0;
            break;
        }
        size_t e = start_code(d->annexb, d->nannexb, s + 3);
        if (e == (size_t)-1) {
            if (!d->flushing) {
                d->apos = s;
                break;
            }
            e = d->nannexb;
        }
        size_t end = e;
        while (end > s + 3 && !d->annexb[end - 1]) end--;
        d->apos = e;
        nal(d, d->annexb + s + 3, end - s - 3);
    }
    if (!d->nout && d->flushing && start_code(d->annexb, d->nannexb, d->apos) == (size_t)-1) {
        d->apos = d->nannexb = 0;
        if (d->cur) finish_picture(d);
        while (bump(d)) {
        }
        d->flushing = false;
    }
}

int hevc_decode(Hevc *d, const uint8_t *data, size_t n, int64_t pts) {
    d->pts = pts;
    if (d->nal_len) {
        size_t o = 0;
        while (o + (size_t)d->nal_len <= n) {
            size_t l = 0;
            for (int k = 0; k < d->nal_len; k++) l = (l << 8) | data[o + k];
            o += (size_t)d->nal_len;
            if (l > n - o) l = n - o;
            nal(d, data + o, l);
            o += l;
        }
        if (d->cur) finish_picture(d);
    } else {
        if (d->apos) { /* what was read goes */
            memmove(d->annexb, d->annexb + d->apos, d->nannexb - d->apos);
            d->nannexb -= d->apos, d->apos = 0;
        }
        if (d->nannexb + n > d->cannexb) {
            size_t c = (d->nannexb + n) * 2;
            uint8_t *b = realloc(d->annexb, c);
            if (!b) return -1;
            d->annexb = b, d->cannexb = c;
        }
        memcpy(d->annexb + d->nannexb, data, n);
        d->nannexb += n;
        pump(d);
    }
    return d->nout;
}

void hevc_flush(Hevc *d) {
    if (!d->nal_len) {
        d->flushing = true;
        pump(d);
        return;
    }
    if (d->cur) finish_picture(d);
    while (bump(d)) {
    }
}

void hevc_reset(Hevc *d) {
    d->cur = NULL;
    for (int i = 0; i < HV_MAX_DPB; i++) {
        HvPic *p = &d->pics[i];
        p->ref = 0, p->output = p->handed = p->busy = false;
    }
    d->nout = 0, d->handed = NULL;
    d->nannexb = d->apos = 0, d->flushing = false;
    d->wait_irap = true;
}

bool hevc_frame(Hevc *d, HevcFrame *f) {
    if (d->handed) {
        HvPic *h = d->handed;
        d->handed = NULL;
        h->handed = false;
        pic_release(d, h);
    }
    if (!d->nout && !d->nal_len) pump(d);
    if (!d->nout) return false;
    HvPic *p = d->out_fifo[0];
    memmove(d->out_fifo, d->out_fifo + 1, (size_t)(d->nout - 1) * sizeof *d->out_fifo);
    d->nout--;
    d->handed = p;
    const HvPic *s = p; /* (the picture keeps its SPS's crop, depth, aspect and rate) */
    int w = p->w - s->crop_l - s->crop_r, h = p->h - s->crop_t - s->crop_b;
    int cw = (w + 1) >> 1, ch = (h + 1) >> 1;
    size_t need = (size_t)w * h + 2 * (size_t)cw * ch;
    if (need > d->nout8) {
        uint8_t *o = realloc(d->out8, need);
        if (!o) return false;
        d->out8 = o, d->nout8 = need;
    }
    int shift = s->depth - 8;
    for (int c = 0; c < 3; c++) {
        int pw = c ? cw : w, ph = c ? ch : h, ox = c ? s->crop_l >> 1 : s->crop_l, oy = c ? s->crop_t >> 1 : s->crop_t;
        uint8_t *o = d->out8 + (c == 0 ? 0 : c == 1 ? (size_t)w * h : (size_t)w * h + (size_t)cw * ch);
        for (int y = 0; y < ph; y++) {
            const px *row = p->plane[c] + (size_t)(y + oy) * p->stride[c] + ox;
            uint8_t *dst = o + (size_t)y * pw;
            if (!shift)
                for (int x = 0; x < pw; x++) dst[x] = (uint8_t)row[x];
            else
                for (int x = 0; x < pw; x++) {
                    int v = (row[x] + (1 << (shift - 1))) >> shift;
                    dst[x] = (uint8_t)(v > 255 ? 255 : v);
                }
        }
    }
    memset(f, 0, sizeof *f);
    f->y = d->out8, f->u = d->out8 + (size_t)w * h, f->v = f->u + (size_t)cw * ch;
    f->stride_y = w, f->stride_c = cw;
    f->y16 = p->plane[0] + (size_t)s->crop_t * p->stride[0] + s->crop_l;
    f->u16 = p->plane[1] + (size_t)(s->crop_t >> 1) * p->stride[1] + (s->crop_l >> 1);
    f->v16 = p->plane[2] + (size_t)(s->crop_t >> 1) * p->stride[2] + (s->crop_l >> 1);
    f->stride16_y = p->stride[0], f->stride16_c = p->stride[1];
    f->width = w, f->height = h, f->bit_depth = s->depth;
    f->par_num = s->sar_num > 0 && s->sar_den > 0 ? s->sar_num : 1, f->par_den = s->sar_num > 0 && s->sar_den > 0 ? s->sar_den : 1;
    f->rate_num = s->rate_num, f->rate_den = s->rate_den;
    f->type = p->type ? p->type : 'P';
    f->pts = p->pts;
    f->damaged = p->damaged;
    return true;
}

bool hevc_info(const Hevc *d, int *width, int *height) {
    const HvSps *s = d->s;
    if (!s)
        for (int i = 0; i < HV_MAX_SPS && !s; i++)
            if (d->sps[i] && d->sps[i]->valid) s = d->sps[i];
    if (!s) return false;
    if (width) *width = s->width - s->crop_l - s->crop_r;
    if (height) *height = s->height - s->crop_t - s->crop_b;
    return true;
}

const char *hevc_unsupported(const Hevc *d) {
    if (d->unsup) return d->unsup;
    if (!d->s) /* nothing decoded yet: the sets known so far */
        for (int i = 0; i < HV_MAX_SPS; i++)
            if (d->sps[i] && d->sps[i]->unsup) return d->sps[i]->unsup;
    return NULL;
}
