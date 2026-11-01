/* modules/av/hevc_ctu.c — HEVC: CABAC and the coding tree (7.3.8, 9.3), motion vector prediction (8.5.3.2) and
   the residuals' scaling (8.6). Each coding unit is predicted and reconstructed as it's read. See hevc.h. */
#include "hevc_int.h"

/* ---- CABAC (9.3.4.3) ---- */

const uint8_t hv_lps[64][4] = {
    {128, 176, 208, 240}, {128, 167, 197, 227}, {128, 158, 187, 216}, {123, 150, 178, 205}, {116, 142, 169, 195},
    {111, 135, 160, 185}, {105, 128, 152, 175}, {100, 122, 144, 166}, {95, 116, 137, 158},  {90, 110, 130, 150},
    {85, 104, 123, 142},  {81, 99, 117, 135},   {77, 94, 111, 128},   {73, 89, 105, 122},   {69, 85, 100, 116},
    {66, 80, 95, 110},    {62, 76, 90, 104},    {59, 72, 86, 99},     {56, 69, 81, 94},     {53, 65, 77, 89},
    {51, 62, 73, 85},     {48, 59, 69, 80},     {46, 56, 66, 76},     {43, 53, 63, 72},     {41, 50, 59, 69},
    {39, 48, 56, 65},     {37, 45, 54, 62},     {35, 43, 51, 59},     {33, 41, 48, 56},     {32, 39, 46, 53},
    {30, 37, 43, 50},     {29, 35, 41, 48},     {27, 33, 39, 45},     {26, 31, 37, 43},     {24, 30, 35, 41},
    {23, 28, 33, 39},     {22, 27, 32, 37},     {21, 26, 30, 35},     {20, 24, 29, 33},     {19, 23, 27, 31},
    {18, 22, 26, 30},     {17, 21, 25, 28},     {16, 20, 23, 27},     {15, 19, 22, 25},     {14, 18, 21, 24},
    {14, 17, 20, 23},     {13, 16, 19, 22},     {12, 15, 18, 21},     {12, 14, 17, 20},     {11, 14, 16, 19},
    {11, 13, 15, 18},     {10, 12, 15, 17},     {10, 12, 14, 16},     {9, 11, 13, 15},      {9, 11, 12, 14},
    {8, 10, 12, 14},      {8, 9, 11, 13},       {7, 9, 11, 12},       {7, 9, 10, 12},       {7, 8, 10, 11},
    {6, 8, 9, 11},        {6, 7, 9, 10},        {6, 7, 8, 9},         {2, 2, 2, 2}};

const uint8_t hv_next_lps[64] = {0,  0,  1,  2,  2,  4,  4,  5,  6,  7,  8,  9,  9,  11, 11, 12,
                                     13, 13, 15, 15, 16, 16, 18, 18, 19, 19, 21, 21, 22, 22, 23, 24,
                                     24, 25, 26, 26, 27, 27, 28, 29, 29, 30, 30, 30, 31, 32, 32, 33,
                                     33, 33, 34, 34, 35, 35, 35, 36, 36, 36, 37, 37, 37, 38, 38, 63};

static inline void refill(HvCabac *c) {
    while (c->cnt < 8) {
        c->value = (c->value << 8) | (c->p < c->end ? *c->p : 0);
        c->p++;
        c->cnt += 8;
    }
}

void hv_cabac_start(HvCabac *c, const uint8_t *p, const uint8_t *end) {
    c->start = c->p = p, c->end = end;
    c->range = 510, c->value = 0, c->cnt = -9;
    refill(c);
}

/* the byte after the arithmetic code that just ended (an end_of_subset_one_bit or pcm_flag of 1) */
static const uint8_t *cabac_after(const HvCabac *c) {
    int64_t bits = (int64_t)(c->p - c->start) * 8 - c->cnt;
    return c->start + (bits + 7) / 8;
}

static inline int dec(HvCabac *c, uint8_t *ctx) {
    int s = *ctx >> 1, mps = *ctx & 1, bin;
    uint32_t lps = hv_lps[s][(c->range >> 6) & 3];
    c->range -= lps;
    uint32_t scaled = c->range << c->cnt;
    if (c->value < scaled) {
        bin = mps;
        *ctx = (uint8_t)(((s < 62 ? s + 1 : 62) << 1) | mps);
        if (c->range < 256) c->range <<= 1, c->cnt--;
    } else {
        c->value -= scaled;
        bin = !mps;
        int n = __builtin_clz(lps) - 23;
        c->range = lps << n, c->cnt -= n;
        *ctx = (uint8_t)((hv_next_lps[s] << 1) | (s ? mps : !mps));
    }
    if (c->cnt < 8) refill(c);
    return bin;
}

static inline int byp(HvCabac *c) {
    c->cnt--;
    uint32_t scaled = c->range << c->cnt;
    int bin = 0;
    if (c->value >= scaled) c->value -= scaled, bin = 1;
    if (c->cnt < 8) refill(c);
    return bin;
}

static inline uint32_t bypn(HvCabac *c, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 1) | (uint32_t)byp(c);
    return v;
}

static int term(HvCabac *c) {
    c->range -= 2;
    uint32_t scaled = c->range << c->cnt;
    if (c->value >= scaled) return 1;
    if (c->range < 256) c->range <<= 1, c->cnt--;
    if (c->cnt < 8) refill(c);
    return 0;
}

static bool overread(const HvCabac *c) { return c->p > c->end + 8; }

/* ---- contexts (9.3.2.2): the standard's initValues by initType, in hevc_int.h's C_* order ---- */

const uint8_t hv_init[3][HV_NCTX] = {
    {153, 200, 139, 141, 157, 154, 154, 154, 154, 154, 154, 154, 184, 154, 154, 154, 184, 63, 154, 154, 154, 154, 154, 154, 
     154, 154, 154, 154, 154, 154, 154, 153, 138, 138, 111, 141, 94, 138, 182, 154, 139, 139, 110, 110, 124, 125, 140, 153, 
     125, 127, 140, 109, 111, 143, 127, 111, 79, 108, 123, 63, 110, 110, 124, 125, 140, 153, 125, 127, 140, 109, 111, 143, 
     127, 111, 79, 108, 123, 63, 91, 171, 134, 141, 111, 111, 125, 110, 110, 94, 124, 108, 124, 107, 125, 141, 179, 153, 
     125, 107, 125, 141, 179, 153, 125, 107, 125, 141, 179, 153, 125, 140, 139, 182, 182, 152, 136, 152, 136, 153, 136, 139, 
     111, 136, 139, 111, 140, 92, 137, 138, 140, 152, 138, 139, 153, 74, 149, 92, 139, 107, 122, 152, 140, 179, 166, 182, 
     140, 227, 122, 197, 138, 153, 136, 167, 152, 152},
    {153, 185, 107, 139, 126, 154, 197, 185, 201, 154, 154, 149, 154, 139, 154, 154, 154, 152, 110, 122, 95, 79, 63, 31, 
     31, 153, 153, 140, 198, 168, 79, 124, 138, 94, 153, 111, 149, 107, 167, 154, 139, 139, 125, 110, 94, 110, 95, 79, 
     125, 111, 110, 78, 110, 111, 111, 95, 94, 108, 123, 108, 125, 110, 94, 110, 95, 79, 125, 111, 110, 78, 110, 111, 
     111, 95, 94, 108, 123, 108, 121, 140, 61, 154, 155, 154, 139, 153, 139, 123, 123, 63, 153, 166, 183, 140, 136, 153, 
     154, 166, 183, 140, 136, 153, 154, 166, 183, 140, 136, 153, 154, 170, 153, 123, 123, 107, 121, 107, 121, 167, 151, 183, 
     140, 151, 183, 140, 154, 196, 196, 167, 154, 152, 167, 182, 182, 134, 149, 136, 153, 121, 136, 137, 169, 194, 166, 167, 
     154, 167, 137, 182, 107, 167, 91, 122, 107, 167},
    {153, 160, 107, 139, 126, 154, 197, 185, 201, 154, 154, 134, 154, 139, 154, 154, 183, 152, 154, 137, 95, 79, 63, 31, 
     31, 153, 153, 169, 198, 168, 79, 224, 167, 122, 153, 111, 149, 92, 167, 154, 139, 139, 125, 110, 124, 110, 95, 94, 
     125, 111, 111, 79, 125, 126, 111, 111, 79, 108, 123, 93, 125, 110, 124, 110, 95, 94, 125, 111, 111, 79, 125, 126, 
     111, 111, 79, 108, 123, 93, 121, 140, 61, 154, 170, 154, 139, 153, 139, 123, 123, 63, 124, 166, 183, 140, 136, 153, 
     154, 166, 183, 140, 136, 153, 154, 166, 183, 140, 136, 153, 154, 170, 153, 138, 138, 122, 121, 122, 121, 167, 151, 183, 
     140, 151, 183, 140, 154, 196, 167, 167, 154, 152, 167, 182, 182, 134, 149, 136, 153, 121, 136, 122, 169, 208, 166, 167, 
     154, 152, 167, 182, 107, 167, 91, 107, 107, 167},
};

void hv_ctx_init(Hevc *d) {
    int t = d->sh.type == SLICE_I ? 0 : d->sh.type == SLICE_P ? (d->sh.cabac_init ? 2 : 1) : (d->sh.cabac_init ? 1 : 2);
    int qp = CLIP3(0, 51, d->sh.qp);
    for (int i = 0; i < HV_NCTX; i++) {
        int v = hv_init[t][i], m = (v >> 4) * 5 - 45, n = ((v & 15) << 3) - 16;
        int pre = CLIP3(1, 126, ((m * qp) >> 4) + n), mps = pre > 63;
        d->ctx[i] = (uint8_t)(((mps ? pre - 64 : 63 - pre) << 1) | mps);
    }
}

/* ---- availability (6.4.1) ---- */

bool hv_avail(const Hevc *d, int xc, int yc, int xn, int yn) {
    const HvSps *s = d->s;
    if (xn < 0 || yn < 0 || xn >= s->width || yn >= s->height) return false;
    int l = s->log2_min_tb;
    if (d->zs[(yn >> l) * d->zw + (xn >> l)] > d->zs[(yc >> l) * d->zw + (xc >> l)]) return false;
    int cn = (yn >> s->log2_ctb) * d->wctb + (xn >> s->log2_ctb), cc = (yc >> s->log2_ctb) * d->wctb + (xc >> s->log2_ctb);
    if (cn != cc) {
        if (d->ctb_addr[cn] < 0 || d->ctb_addr[cn] != d->ctb_addr[cc]) return false;
        if (d->tile_id[d->rs2ts[cn]] != d->tile_id[d->rs2ts[cc]]) return false;
    }
    return true;
}

/* ---- the coding unit being read ---- */

enum { MODE_INTER, MODE_INTRA, MODE_SKIP };

typedef struct {
    int x0, y0, log2, size, depth;
    int pred, part;
    bool bypass, pcm, intra_split, merge0;
    int ipm[4], ipm_c;
    int max_depth;
    int qp_y, qp_c[2]; /* Qp'Y, Qp'Cb, Qp'Cr (with the bit depth's offset) */
} Cu;

#define MINCB(d, x, y) ((size_t)((y) >> (d)->s->log2_min_cb) * (d)->wmin + ((x) >> (d)->s->log2_min_cb))
#define AT4(d, x, y) ((size_t)((y) >> 2) * (d)->w4 + ((x) >> 2))

static const uint8_t QPC[14] = {29, 30, 31, 32, 33, 33, 34, 34, 35, 35, 36, 36, 37, 37}; /* qPi 30..43 */

static int chroma_qp(int qpi) { return qpi < 30 ? qpi : qpi > 43 ? qpi - 6 : QPC[qpi - 30]; }

static void cu_qp(Hevc *d, Cu *cu) {
    int bd = 6 * (d->s->depth - 8);
    int qy = ((d->qg_pred + d->cu_qp_delta + 52 + 2 * bd) % (52 + bd)) - bd;
    d->qp_y = qy;
    cu->qp_y = qy + bd;
    cu->qp_c[0] = chroma_qp(CLIP3(-bd, 57, qy + d->p->cb_off + d->sh.cb_off)) + bd;
    cu->qp_c[1] = chroma_qp(CLIP3(-bd, 57, qy + d->p->cr_off + d->sh.cr_off)) + bd;
}

/* ---- SAO syntax (7.3.8.3) ---- */

static void sao_syntax(Hevc *d, int cx, int cy, int rs) {
    HvCabac *c = &d->cc;
    HvSao *o = &d->sao[rs];
    memset(o, 0, sizeof *o);
    int left = 0, up = 0;
    if (cx > 0 && rs > d->slice_addr_rs && d->tile_id[d->rs2ts[rs]] == d->tile_id[d->rs2ts[rs - 1]])
        left = dec(c, &d->ctx[C_SAO_MERGE]);
    if (cy > 0 && !left && rs - d->wctb >= d->slice_addr_rs && d->tile_id[d->rs2ts[rs]] == d->tile_id[d->rs2ts[rs - d->wctb]])
        up = dec(c, &d->ctx[C_SAO_MERGE]);
    if (left || up) {
        *o = d->sao[left ? rs - 1 : rs - d->wctb];
        return;
    }
    int cmax = (1 << ((d->s->depth < 10 ? d->s->depth : 10) - 5)) - 1;
    for (int k = 0; k < 3; k++) {
        if ((k == 0 && !d->sh.sao_luma) || (k > 0 && !d->sh.sao_chroma)) continue;
        if (k < 2) o->type[k] = !dec(c, &d->ctx[C_SAO_TYPE]) ? 0 : byp(c) ? 2 : 1;
        else o->type[2] = o->type[1];
        if (!o->type[k]) continue;
        int abs[4];
        for (int i = 0; i < 4; i++) {
            abs[i] = 0;
            while (abs[i] < cmax && byp(c)) abs[i]++;
        }
        if (o->type[k] == 1) {
            for (int i = 0; i < 4; i++) o->off[k][i + 1] = (int8_t)(abs[i] && byp(c) ? -abs[i] : abs[i]);
            o->band[k] = (uint8_t)bypn(c, 5);
        } else {
            o->off[k][1] = (int8_t)abs[0], o->off[k][2] = (int8_t)abs[1];
            o->off[k][3] = (int8_t)-abs[2], o->off[k][4] = (int8_t)-abs[3];
            if (k == 0) o->eo[0] = (uint8_t)bypn(c, 2);
            if (k == 1) o->eo[1] = (uint8_t)bypn(c, 2);
            if (k == 2) o->eo[2] = o->eo[1];
        }
    }
}

/* ---- residuals (7.3.8.11) ---- */

static const uint8_t SCAN4[3][16][2] = {
    {{0, 0}, {0, 1}, {1, 0}, {0, 2}, {1, 1}, {2, 0}, {0, 3}, {1, 2}, {2, 1}, {3, 0}, {1, 3}, {2, 2}, {3, 1}, {2, 3}, {3, 2}, {3, 3}},
    {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {0, 1}, {1, 1}, {2, 1}, {3, 1}, {0, 2}, {1, 2}, {2, 2}, {3, 2}, {0, 3}, {1, 3}, {2, 3}, {3, 3}},
    {{0, 0}, {0, 1}, {0, 2}, {0, 3}, {1, 0}, {1, 1}, {1, 2}, {1, 3}, {2, 0}, {2, 1}, {2, 2}, {2, 3}, {3, 0}, {3, 1}, {3, 2}, {3, 3}}};

static const uint8_t SCAN2[3][4][2] = {{{0, 0}, {0, 1}, {1, 0}, {1, 1}}, {{0, 0}, {1, 0}, {0, 1}, {1, 1}},
                                       {{0, 0}, {0, 1}, {1, 0}, {1, 1}}};

static const uint8_t CTX_IDX_MAP[16] = {0, 1, 4, 5, 2, 3, 4, 5, 6, 6, 8, 8, 7, 7, 8, 8};

/* sub-block scans for 16x16 and 32x32 (diagonal only: other scans are only for 4x4 and 8x8) */
static void diag_scan(uint8_t (*out)[2], int n) {
    int i = 0, x = 0, y = 0;
    while (i < n * n) {
        while (y >= 0) {
            if (x < n && y < n) out[i][0] = (uint8_t)x, out[i][1] = (uint8_t)y, i++;
            y--, x++;
        }
        y = x, x = 0;
    }
}

static const int LEVEL_SCALE[6] = {40, 45, 51, 57, 64, 72};

/* adds the residual of one transform block to the picture; x0, y0 in the component's samples */
static bool residual(Hevc *d, Cu *cu, int x0, int y0, int log2, int k, int pred_mode) {
    HvCabac *c = &d->cc;
    const HvPps *p = d->p;
    int n = 1 << log2;
    int16_t *coef = d->coeff;
    memset(coef, 0, sizeof(int16_t) * (size_t)(n * n));
    bool ts = false;
    if (p->transform_skip && !cu->bypass && log2 == 2) ts = dec(c, &d->ctx[C_TSKIP + (k ? 1 : 0)]);
    int off, shift, max = (log2 << 1) - 1;
    if (!k) off = 3 * (log2 - 2) + ((log2 - 1) >> 2), shift = (log2 + 1) >> 2;
    else off = 15, shift = log2 - 2;
    int px_ = 0, py_ = 0;
    while (px_ < max && dec(c, &d->ctx[C_LAST_X + off + (px_ >> shift)])) px_++;
    while (py_ < max && dec(c, &d->ctx[C_LAST_Y + off + (py_ >> shift)])) py_++;
    int lx = px_, ly = py_;
    if (px_ > 3) {
        int b = (px_ >> 1) - 1;
        lx = (1 << b) * (2 + (px_ & 1)) + (int)bypn(c, b);
    }
    if (py_ > 3) {
        int b = (py_ >> 1) - 1;
        ly = (1 << b) * (2 + (py_ & 1)) + (int)bypn(c, b);
    }
    int scan = 0;
    if (cu->pred == MODE_INTRA && (log2 == 2 || (log2 == 3 && k == 0))) {
        if (pred_mode >= 6 && pred_mode <= 14) scan = 2;
        else if (pred_mode >= 22 && pred_mode <= 30) scan = 1;
    }
    if (scan == 2) {
        int t = lx;
        lx = ly, ly = t;
    }
    if (lx >= n || ly >= n) return false;
    int lsb = log2 - 2, nsb = 1 << lsb;
    uint8_t big[64][2];
    const uint8_t (*sbscan)[2];
    if (lsb == 0) sbscan = SCAN2[0]; /* (one sub-block) */
    else if (lsb == 1) sbscan = SCAN2[scan];
    else diag_scan(big, nsb), sbscan = (const uint8_t (*)[2])big;
    const uint8_t (*sc)[2] = SCAN4[scan];
    /* where the last coefficient is */
    int last_sb = 0, last_pos = 0;
    for (int i = 0; i < nsb * nsb; i++)
        if (sbscan[i][0] == lx >> 2 && sbscan[i][1] == ly >> 2) last_sb = i;
    for (int i = 0; i < 16; i++)
        if (sc[i][0] == (lx & 3) && sc[i][1] == (ly & 3)) last_pos = i;
    uint8_t csbf[8][8];
    memset(csbf, 0, sizeof csbf);
    int c1 = 1;
    bool first_sb = true;
    bool any_ac = false;
    int qp = !k ? cu->qp_y : cu->qp_c[k - 1];
    for (int i = last_sb; i >= 0; i--) {
        int xs = sbscan[i][0], ys = sbscan[i][1];
        bool infer_dc = false;
        if (i < last_sb && i > 0) {
            int inc = 0;
            if (xs < nsb - 1) inc += csbf[xs + 1][ys];
            if (ys < nsb - 1) inc += csbf[xs][ys + 1];
            csbf[xs][ys] = (uint8_t)dec(c, &d->ctx[C_CSBF + (inc ? 1 : 0) + (k ? 2 : 0)]);
            infer_dc = true;
        } else {
            csbf[xs][ys] = 1;
        }
        int nsig = 0, pos[16];
        int start = i == last_sb ? last_pos - 1 : 15;
        if (i == last_sb) pos[nsig++] = last_pos;
        int prev = 0;
        if (xs < nsb - 1) prev += csbf[xs + 1][ys];
        if (ys < nsb - 1) prev += csbf[xs][ys + 1] << 1;
        for (int m = start; m >= 0 && csbf[xs][ys]; m--) {
            int xc = (xs << 2) + sc[m][0], yc = (ys << 2) + sc[m][1];
            if (m > 0 || !infer_dc) {
                int sctx;
                if (log2 == 2) sctx = CTX_IDX_MAP[(yc << 2) + xc];
                else if (xc + yc == 0) sctx = 0;
                else {
                    int xp = xc & 3, yp = yc & 3;
                    if (prev == 0) sctx = xp + yp == 0 ? 2 : xp + yp < 3 ? 1 : 0;
                    else if (prev == 1) sctx = yp == 0 ? 2 : yp == 1 ? 1 : 0;
                    else if (prev == 2) sctx = xp == 0 ? 2 : xp == 1 ? 1 : 0;
                    else sctx = 2;
                    if (!k) {
                        if (xs + ys > 0) sctx += 3;
                        sctx += log2 == 3 ? (scan == 0 ? 9 : 15) : 21;
                    } else {
                        sctx += log2 == 3 ? 9 : 12;
                    }
                }
                if (dec(c, &d->ctx[C_SIG + (k ? 27 : 0) + sctx])) {
                    pos[nsig++] = m;
                    infer_dc = false;
                }
            } else {
                pos[nsig++] = 0; /* inferred: the sub-block's only coefficient is its first */
            }
        }
        if (!nsig) continue;
        /* levels */
        int ctx_set = (i == 0 || k) ? 0 : 2;
        if (!first_sb && c1 == 0) ctx_set++;
        first_sb = false;
        c1 = 1;
        uint8_t g1[16] = {0};
        int first_g1 = -1;
        for (int j = 0; j < nsig && j < 8; j++) {
            g1[j] = (uint8_t)dec(c, &d->ctx[C_G1 + (k ? 16 : 0) + ctx_set * 4 + c1]);
            if (g1[j]) {
                c1 = 0;
                if (first_g1 < 0) first_g1 = j;
            } else if (c1 > 0 && c1 < 3) {
                c1++;
            }
        }
        uint8_t g2 = 0;
        if (first_g1 >= 0) g2 = (uint8_t)dec(c, &d->ctx[C_G2 + (k ? 4 : 0) + ctx_set]);
        bool hidden = p->sign_hiding && !cu->bypass && pos[0] - pos[nsig - 1] > 3;
        int sgn[16];
        for (int j = 0; j < nsig; j++) sgn[j] = (j < nsig - 1 || !hidden) ? byp(c) : 0;
        int rice = 0, sum = 0;
        for (int j = 0; j < nsig; j++) {
            int base = 1 + (j < 8 ? g1[j] : 0) + (j == first_g1 ? g2 : 0);
            int level = base;
            if (base == ((j < 8) ? (j == first_g1 ? 3 : 2) : 1)) {
                int pre = 0;
                while (pre < 32 && byp(c)) pre++;
                int rem;
                if (pre <= 3) rem = (pre << rice) + (int)bypn(c, rice);
                else {
                    int nb = pre - 3 + rice;
                    if (nb > 24) return false;
                    rem = (((1 << (pre - 3)) + 3 - 1) << rice) + (int)bypn(c, nb);
                }
                level = base + rem;
                if (level > 3 * (1 << rice)) rice = rice < 4 ? rice + 1 : 4;
            }
            int m = pos[j], xc = (xs << 2) + sc[m][0], yc = (ys << 2) + sc[m][1];
            int v = sgn[j] ? -level : level;
            if (hidden) {
                sum += level;
                if (j == nsig - 1 && (sum & 1)) v = -v;
            }
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            coef[yc * n + xc] = (int16_t)v;
            if (xc || yc) any_ac = true;
        }
        if (overread(c)) return false;
    }
    /* scaling (8.6.2) and the transform (8.6.4) */
    int32_t res[32 * 32];
    if (cu->bypass) {
        for (int i = 0; i < n * n; i++) res[i] = coef[i];
    } else {
        int depth = d->s->depth, bdshift = depth + log2 - 5;
        int ls = LEVEL_SCALE[qp % 6] << (qp / 6);
        bool flat = d->scale_flat || (ts && n > 4);
        const int16_t *m = flat ? NULL : d->scale[log2 - 2][(cu->pred == MODE_INTRA ? 0 : 3) + k];
        int64_t rnd = (int64_t)1 << (bdshift - 1);
        for (int i = 0; i < n * n; i++) {
            if (!coef[i]) continue;
            int64_t v = ((int64_t)coef[i] * (flat ? 16 : m[i]) * ls + rnd) >> bdshift;
            coef[i] = (int16_t)CLIP3(-32768, 32767, v);
        }
        if (ts) {
            int sh = 20 - depth;
            for (int i = 0; i < n * n; i++) res[i] = (coef[i] * 128 + (1 << (sh - 1))) >> sh;
        } else if (!any_ac && !(cu->pred == MODE_INTRA && !k && log2 == 2)) {
            hv_transform_dc(res, coef[0], log2, depth);
        } else {
            hv_transform(res, coef, log2, cu->pred == MODE_INTRA && !k && log2 == 2, depth);
        }
    }
    HvPic *pic = d->cur;
    px *dst = pic->plane[k] + (size_t)y0 * pic->stride[k] + x0;
    int maxv = (1 << d->s->depth) - 1;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            int v = dst[(size_t)y * pic->stride[k] + x] + res[y * n + x];
            dst[(size_t)y * pic->stride[k] + x] = (px)CLIP3(0, maxv, v);
        }
    return true;
}

/* ---- intra modes (8.4.2) ---- */

static int cand_mode(Hevc *d, int xp, int yp, int xn, int yn, bool above) {
    if (!hv_avail(d, xp, yp, xn, yn)) return 1;
    size_t a = AT4(d, xn, yn);
    if (d->cur->mv[a].pred || (d->flags4[a] & F4_PCM)) return 1;
    if (above && yn < ((yp >> d->s->log2_ctb) << d->s->log2_ctb)) return 1;
    return d->ipm[a];
}

static void set_ipm(Hevc *d, int x, int y, int size, int mode) {
    for (int j = 0; j < size; j += 4)
        memset(&d->ipm[AT4(d, x, y + j)], mode, (size_t)(size >> 2));
}

/* ---- deblocking edges (8.7.2.2-8.7.2.4): the boundary strengths of a block's top and left edges ---- */

static bool edge_ok(Hevc *d, int x, int y, int xn, int yn) {
    /* filterEdgeFlag for an edge between (xn, yn) and the current block at (x, y) */
    const HvSps *s = d->s;
    if (xn < 0 || yn < 0) return false;
    int cn = (yn >> s->log2_ctb) * d->wctb + (xn >> s->log2_ctb), cc = (y >> s->log2_ctb) * d->wctb + (x >> s->log2_ctb);
    if (cn == cc) return true;
    if (d->ctb_addr[cn] < 0) return false; /* (a slice that never came) */
    if (!d->p->lf_tiles && d->tile_id[d->rs2ts[cn]] != d->tile_id[d->rs2ts[cc]]) return false;
    if (!d->sh.lf_slices && d->ctb_addr[cn] != d->ctb_addr[cc]) return false;
    return true;
}

static int mv_bs(const Hevc *d, const HvMv *a, const HvMv *b, const HvRefs *ra, const HvRefs *rb) {
    /* 1 when the two blocks' motion differs enough (8.7.2.4), else 0; refs compared as pictures (POC) */
    int na = (a->pred & 1) + (a->pred >> 1), nb = (b->pred & 1) + (b->pred >> 1);
    if (na != nb) return 1;
    int pa[2], pb[2];
    const int16_t *va[2], *vb[2];
    int ka = 0, kb = 0;
    for (int l = 0; l < 2; l++) {
        if (a->pred & (1 << l)) pa[ka] = ra->poc[l][a->ref[l]], va[ka++] = a->mv[l];
        if (b->pred & (1 << l)) pb[kb] = rb->poc[l][b->ref[l]], vb[kb++] = b->mv[l];
    }
    (void)d;
#define FAR(u, v) (abs((u)[0] - (v)[0]) >= 4 || abs((u)[1] - (v)[1]) >= 4)
    if (na == 1) return pa[0] != pb[0] || FAR(va[0], vb[0]);
    if (!((pa[0] == pb[0] && pa[1] == pb[1]) || (pa[0] == pb[1] && pa[1] == pb[0]))) return 1;
    if (pa[0] != pa[1]) {
        if (pa[0] == pb[0]) return FAR(va[0], vb[0]) || FAR(va[1], vb[1]);
        return FAR(va[0], vb[1]) || FAR(va[1], vb[0]);
    }
    return (FAR(va[0], vb[0]) || FAR(va[1], vb[1])) && (FAR(va[0], vb[1]) || FAR(va[1], vb[0]));
#undef FAR
}

static int bs_at(Hevc *d, int xp, int yp, int xq, int yq, bool tu_edge) {
    const HvMv *p = &d->cur->mv[AT4(d, xp, yp)], *q = &d->cur->mv[AT4(d, xq, yq)];
    if (!p->pred || !q->pred) return 2;
    if (tu_edge && ((d->flags4[AT4(d, xp, yp)] | d->flags4[AT4(d, xq, yq)]) & F4_CBF)) return 1;
    const HvRefs *rp = &d->cur->refs[d->cur->ctb_slice[(yp >> d->s->log2_ctb) * d->wctb + (xp >> d->s->log2_ctb)]];
    const HvRefs *rq = &d->cur->refs[d->slice_no];
    return mv_bs(d, p, q, rp, rq);
}

/* the edges of a transform block (x0, y0, size) - and, inside an inter block, the prediction edges */
static void edges(Hevc *d, int x0, int y0, int size, bool inter) {
    if (d->sh.deblock_off) return;
    const HvSps *s = d->s;
    int bw = (s->width + 7) >> 3;
    if (!(x0 & 7) && edge_ok(d, x0, y0, x0 - 1, y0))
        for (int y = y0; y < y0 + size && y < s->height; y += 4) d->bs_v[(y >> 2) * bw + (x0 >> 3)] = (uint8_t)bs_at(d, x0 - 1, y, x0, y, true);
    if (!(y0 & 7) && edge_ok(d, x0, y0, x0, y0 - 1))
        for (int x = x0; x < x0 + size && x < s->width; x += 4) d->bs_h[(y0 >> 3) * d->w4 + (x >> 2)] = (uint8_t)bs_at(d, x, y0 - 1, x, y0, true);
    if (inter && size > 8) /* inside one transform block only the prediction blocks' edges can differ */
        for (int y = y0; y < y0 + size && y < s->height; y += 4)
            for (int x = x0; x < x0 + size && x < s->width; x += 4) {
                if (!(x & 7) && x > x0) d->bs_v[(y >> 2) * bw + (x >> 3)] = (uint8_t)bs_at(d, x - 1, y, x, y, false);
                if (!(y & 7) && y > y0) d->bs_h[(y >> 3) * d->w4 + (x >> 2)] = (uint8_t)bs_at(d, x, y - 1, x, y, false);
            }
}

/* ---- motion (8.5.3.2) ---- */

static const HvMv *mv_at(const Hevc *d, int x, int y) { return &d->cur->mv[AT4(d, x, y)]; }

/* 6.4.2: prediction block availability */
static bool pb_avail(Hevc *d, const Cu *cu, int xp, int yp, int w, int h, int part, int xn, int yn) {
    bool avail;
    if (!(cu->x0 <= xn && yn >= cu->y0 && xn < cu->x0 + cu->size && yn < cu->y0 + cu->size))
        avail = hv_avail(d, xp, yp, xn, yn);
    else if ((w << 1) == cu->size && (h << 1) == cu->size && part == 1 && cu->y0 + h <= yn && cu->x0 + w > xn)
        avail = false;
    else
        avail = true;
    return avail && mv_at(d, xn, yn)->pred;
}

static int16_t scale_mv(int mv, int td, int tb) {
    td = CLIP3(-128, 127, td), tb = CLIP3(-128, 127, tb);
    if (!td) return (int16_t)mv;
    int tx = (16384 + (abs(td) >> 1)) / td;
    int dsf = CLIP3(-4096, 4095, (tb * tx + 32) >> 6);
    int prod = dsf * mv;
    int v = prod >= 0 ? (prod + 127) >> 8 : -((-prod + 127) >> 8);
    return (int16_t)CLIP3(-32768, 32767, v);
}

static bool col_mv(Hevc *d, HvPic *col, int x, int y, int lx, int ref_idx, int16_t out[2]) {
    if (x >= d->s->width || y >= d->s->height) return false;
    const HvMv *m = &col->mv[AT4(d, x, y)];
    if (!m->pred) return false;
    int si = col->ctb_slice[(y >> d->s->log2_ctb) * d->wctb + (x >> d->s->log2_ctb)];
    if (si < 0 || si >= col->nrefs) return false;
    const HvRefs *rf = &col->refs[si];
    int list = !(m->pred & 1) ? 1 : m->pred == 1 ? 0 : d->lowdelay ? lx : (d->sh.col_l0 ? 1 : 0);
    int ri = m->ref[list];
    if (ri < 0 || ri >= rf->n[list]) return false;
    HvPic *target = d->refpic[lx][ref_idx];
    bool cur_lt = target->ref == 2;
    if (rf->lt[list][ri] != cur_lt) return false;
    int col_diff = col->poc - rf->poc[list][ri], cur_diff = d->poc - target->poc;
    if (cur_lt || col_diff == cur_diff) out[0] = m->mv[list][0], out[1] = m->mv[list][1];
    else out[0] = scale_mv(m->mv[list][0], col_diff, cur_diff), out[1] = scale_mv(m->mv[list][1], col_diff, cur_diff);
    return true;
}

static bool temporal(Hevc *d, int xp, int yp, int w, int h, int lx, int ref_idx, int16_t out[2]) {
    if (!d->sh.tmvp) return false;
    HvPic *col = d->sh.type == SLICE_B && !d->sh.col_l0 ? d->refpic[1][d->sh.col_ref] : d->refpic[0][d->sh.col_ref];
    if (!col || col->missing) return false;
    int xb = xp + w, yb = yp + h;
    if ((yp >> d->s->log2_ctb) == (yb >> d->s->log2_ctb) && yb < d->s->height && xb < d->s->width &&
        col_mv(d, col, (xb >> 4) << 4, (yb >> 4) << 4, lx, ref_idx, out))
        return true;
    int xc = xp + (w >> 1), yc = yp + (h >> 1);
    return col_mv(d, col, (xc >> 4) << 4, (yc >> 4) << 4, lx, ref_idx, out);
}

static bool same_motion(const HvMv *a, const HvMv *b) {
    if (a->pred != b->pred) return false;
    for (int l = 0; l < 2; l++)
        if ((a->pred & (1 << l)) && (a->mv[l][0] != b->mv[l][0] || a->mv[l][1] != b->mv[l][1] || a->ref[l] != b->ref[l]))
            return false;
    return true;
}

static const uint8_t COMB[12][2] = {{0, 1}, {1, 0}, {0, 2}, {2, 0}, {1, 2}, {2, 1},
                                    {0, 3}, {3, 0}, {1, 3}, {3, 1}, {2, 3}, {3, 2}};

static void merge(Hevc *d, Cu *cu, int xp, int yp, int w, int h, int part, int idx, HvMv *out) {
    int ow = w, oh = h;
    const HvPps *p = d->p;
    bool single = p->log2_merge > 2 && cu->size == 8;
    if (single) xp = cu->x0, yp = cu->y0, w = h = 8, part = 0;
    HvMv list[5];
    int n = 0, lm = p->log2_merge;
#define MER(xn, yn) ((xp >> lm) == ((xn) >> lm) && (yp >> lm) == ((yn) >> lm))
    int xa1 = xp - 1, ya1 = yp + h - 1, xb1 = xp + w - 1, yb1 = yp - 1, xb0 = xp + w, yb0 = yp - 1;
    int xa0 = xp - 1, ya0 = yp + h, xb2 = xp - 1, yb2 = yp - 1;
    bool a1 = !(part == 1 && (cu->part == PART_Nx2N || cu->part == PART_nLx2N || cu->part == PART_nRx2N)) &&
              !MER(xa1, ya1) && pb_avail(d, cu, xp, yp, w, h, part, xa1, ya1);
    /* (a candidate is compared with the ones before it when they're AVAILABLE - also one left out as a copy -
       as HM and FFmpeg do) */
    if (a1) list[n++] = *mv_at(d, xa1, ya1);
    bool b1 = !(part == 1 && (cu->part == PART_2NxN || cu->part == PART_2NxnU || cu->part == PART_2NxnD)) &&
              !MER(xb1, yb1) && pb_avail(d, cu, xp, yp, w, h, part, xb1, yb1);
    if (b1 && !(a1 && same_motion(mv_at(d, xb1, yb1), mv_at(d, xa1, ya1)))) list[n++] = *mv_at(d, xb1, yb1);
    bool b0 = !MER(xb0, yb0) && pb_avail(d, cu, xp, yp, w, h, part, xb0, yb0);
    if (b0 && !(b1 && same_motion(mv_at(d, xb0, yb0), mv_at(d, xb1, yb1)))) list[n++] = *mv_at(d, xb0, yb0);
    bool a0 = !MER(xa0, ya0) && pb_avail(d, cu, xp, yp, w, h, part, xa0, ya0);
    if (a0 && !(a1 && same_motion(mv_at(d, xa0, ya0), mv_at(d, xa1, ya1)))) list[n++] = *mv_at(d, xa0, ya0);
    bool b2 = n < 4 && !MER(xb2, yb2) && pb_avail(d, cu, xp, yp, w, h, part, xb2, yb2);
    if (b2 && !(a1 && same_motion(mv_at(d, xb2, yb2), mv_at(d, xa1, ya1))) &&
        !(b1 && same_motion(mv_at(d, xb2, yb2), mv_at(d, xb1, yb1))))
        list[n++] = *mv_at(d, xb2, yb2);
#undef MER
    int maxc = d->sh.max_merge;
    if (n < maxc && idx >= n) {
        HvMv t = {{{0, 0}, {0, 0}}, {-1, -1}, 0, 0};
        if (temporal(d, xp, yp, w, h, 0, 0, t.mv[0])) t.pred |= 1, t.ref[0] = 0;
        if (d->sh.type == SLICE_B && temporal(d, xp, yp, w, h, 1, 0, t.mv[1])) t.pred |= 2, t.ref[1] = 0;
        if (t.pred) list[n++] = t;
    }
    int orig = n;
    if (d->sh.type == SLICE_B && orig > 1 && orig < maxc && idx >= n) {
        for (int ci = 0; ci < orig * (orig - 1) && n < maxc && idx >= n; ci++) {
            const HvMv *l0 = &list[COMB[ci][0]], *l1 = &list[COMB[ci][1]];
            if ((l0->pred & 1) && (l1->pred & 2) &&
                (d->refpic[0][l0->ref[0]]->poc != d->refpic[1][l1->ref[1]]->poc || l0->mv[0][0] != l1->mv[1][0] ||
                 l0->mv[0][1] != l1->mv[1][1])) {
                HvMv c = {{{l0->mv[0][0], l0->mv[0][1]}, {l1->mv[1][0], l1->mv[1][1]}}, {l0->ref[0], l1->ref[1]}, 3, 0};
                list[n++] = c;
            }
        }
    }
    if (idx >= n) {
        int nref = d->sh.type == SLICE_P ? d->sh.nref[0] : (d->sh.nref[0] < d->sh.nref[1] ? d->sh.nref[0] : d->sh.nref[1]);
        int zi = idx - n; /* (the zero candidates count up from there) */
        HvMv z = {{{0, 0}, {0, 0}}, {-1, -1}, 1, 0};
        z.ref[0] = (int8_t)(zi < nref ? zi : 0);
        if (d->sh.type == SLICE_B) z.pred = 3, z.ref[1] = z.ref[0];
        *out = z;
    } else {
        *out = list[idx];
    }
    if (!(out->pred & 1)) out->ref[0] = -1, out->mv[0][0] = out->mv[0][1] = 0;
    if (!(out->pred & 2)) out->ref[1] = -1, out->mv[1][0] = out->mv[1][1] = 0;
    if (out->pred == 3 && ow + oh == 12) out->pred = 1, out->ref[1] = -1, out->mv[1][0] = out->mv[1][1] = 0;
}

/* one spatial AMVP candidate's vector for list lx / reference ref_idx; scaled: the long-term rule and scaling */
static bool amvp_cand(Hevc *d, int xn, int yn, int lx, int ref_idx, bool scaled, int16_t out[2]) {
    const HvMv *m = mv_at(d, xn, yn);
    HvPic *target = d->refpic[lx][ref_idx];
    for (int k = 0; k < 2; k++) {
        int l = k ? !lx : lx;
        if (!(m->pred & (1 << l))) continue;
        HvPic *r = d->refpic[l][m->ref[l]];
        if (!r) continue;
        if (!scaled) {
            if (r->poc == target->poc) {
                out[0] = m->mv[l][0], out[1] = m->mv[l][1];
                return true;
            }
        } else if ((r->ref == 2) == (target->ref == 2)) {
            out[0] = m->mv[l][0], out[1] = m->mv[l][1];
            if (target->ref != 2) {
                int td = d->poc - r->poc, tb = d->poc - target->poc;
                out[0] = scale_mv(out[0], td, tb), out[1] = scale_mv(out[1], td, tb);
            }
            return true;
        }
    }
    return false;
}

static void amvp(Hevc *d, Cu *cu, int xp, int yp, int w, int h, int part, int lx, int ref_idx, int flag, int16_t out[2]) {
    int xa[2] = {xp - 1, xp - 1}, ya[2] = {yp + h, yp + h - 1};
    int xb[3] = {xp + w, xp + w - 1, xp - 1}, yb[3] = {yp - 1, yp - 1, yp - 1};
    bool av[2], bv[3];
    for (int k = 0; k < 2; k++) av[k] = pb_avail(d, cu, xp, yp, w, h, part, xa[k], ya[k]);
    for (int k = 0; k < 3; k++) bv[k] = pb_avail(d, cu, xp, yp, w, h, part, xb[k], yb[k]);
    bool scaled = av[0] || av[1];
    int16_t mva[2] = {0, 0}, mvb[2] = {0, 0};
    bool fa = false, fb = false;
    for (int k = 0; k < 2 && !fa; k++)
        if (av[k]) fa = amvp_cand(d, xa[k], ya[k], lx, ref_idx, false, mva);
    for (int k = 0; k < 2 && !fa; k++)
        if (av[k]) fa = amvp_cand(d, xa[k], ya[k], lx, ref_idx, true, mva);
    for (int k = 0; k < 3 && !fb; k++)
        if (bv[k]) fb = amvp_cand(d, xb[k], yb[k], lx, ref_idx, false, mvb);
    if (!scaled) {
        if (fb) fa = true, mva[0] = mvb[0], mva[1] = mvb[1];
        fb = false;
        for (int k = 0; k < 3 && !fb; k++)
            if (bv[k]) fb = amvp_cand(d, xb[k], yb[k], lx, ref_idx, true, mvb);
    }
    int16_t list[3][2];
    int n = 0;
    if (fa) list[n][0] = mva[0], list[n][1] = mva[1], n++;
    if (fb && !(fa && mva[0] == mvb[0] && mva[1] == mvb[1])) list[n][0] = mvb[0], list[n][1] = mvb[1], n++;
    if (n < 2 && flag == n) {
        int16_t t[2];
        if (temporal(d, xp, yp, w, h, lx, ref_idx, t)) list[n][0] = t[0], list[n][1] = t[1], n++;
    }
    while (n < 2) list[n][0] = list[n][1] = 0, n++;
    out[0] = list[flag][0], out[1] = list[flag][1];
}

/* k-th order Exp-Golomb (bypass bins). Every value read so is at most 2^15 in a stream that keeps the rules (a vector
   difference, a QP delta): a longer prefix is a damaged one, stopped there. */
static int eg(HvCabac *c, int k) {
    int v = 0;
    while (byp(c)) {
        v += 1 << k;
        if (++k > 16) return v;
    }
    return v + (int)bypn(c, k);
}

static void mvd(HvCabac *c, uint8_t *ctx, int out[2]) {
    int g0x = dec(c, &ctx[C_MVD_G0]), g0y = dec(c, &ctx[C_MVD_G0]);
    int g1x = g0x ? dec(c, &ctx[C_MVD_G1]) : 0, g1y = g0y ? dec(c, &ctx[C_MVD_G1]) : 0;
    out[0] = out[1] = 0;
    if (g0x) {
        int a = 1 + (g1x ? 1 + eg(c, 1) : 0);
        out[0] = byp(c) ? -a : a;
    }
    if (g0y) {
        int a = 1 + (g1y ? 1 + eg(c, 1) : 0);
        out[1] = byp(c) ? -a : a;
    }
}

static void store_mv(Hevc *d, int x, int y, int w, int h, const HvMv *mv) {
    for (int j = y; j < y + h; j += 4)
        for (int i = x; i < x + w; i += 4) d->cur->mv[AT4(d, i, j)] = *mv;
}

static bool prediction_unit(Hevc *d, Cu *cu, int x, int y, int w, int h, int part) {
    HvCabac *c = &d->cc;
    HvMv mv = {{{0, 0}, {0, 0}}, {-1, -1}, 0, 0};
    bool mrg = cu->pred == MODE_SKIP ? true : dec(c, &d->ctx[C_MERGE_FLAG]);
    if (part == 0) cu->merge0 = mrg;
    if (mrg) {
        int idx = 0;
        if (d->sh.max_merge > 1 && dec(c, &d->ctx[C_MERGE_IDX])) {
            idx = 1;
            while (idx < d->sh.max_merge - 1 && byp(c)) idx++;
        }
        merge(d, cu, x, y, w, h, part, idx, &mv);
    } else {
        int dir = 1;
        if (d->sh.type == SLICE_B) {
            if (w + h != 12 && dec(c, &d->ctx[C_INTER_DIR + cu->depth])) dir = 3;
            else dir = dec(c, &d->ctx[C_INTER_DIR + 4]) ? 2 : 1;
        }
        int ref[2] = {-1, -1}, md[2][2] = {{0, 0}, {0, 0}}, flag[2] = {0, 0};
        for (int l = 0; l < 2; l++) {
            if (!(dir & (1 << l))) continue;
            ref[l] = 0;
            int max = d->sh.nref[l] - 1;
            while (ref[l] < max && (ref[l] < 2 ? dec(c, &d->ctx[C_REF_IDX + ref[l]]) : byp(c))) ref[l]++;
            if (l == 1 && d->sh.mvd_l1_zero && dir == 3) md[1][0] = md[1][1] = 0;
            else mvd(c, d->ctx, md[l]);
            flag[l] = dec(c, &d->ctx[C_MVP]);
        }
        mv.pred = (uint8_t)dir;
        for (int l = 0; l < 2; l++) {
            if (!(dir & (1 << l))) continue;
            if (!d->refpic[l][ref[l]]) return false;
            int16_t pred[2];
            amvp(d, cu, x, y, w, h, part, l, ref[l], flag[l], pred);
            mv.mv[l][0] = (int16_t)(uint16_t)(pred[0] + md[l][0]);
            mv.mv[l][1] = (int16_t)(uint16_t)(pred[1] + md[l][1]);
            mv.ref[l] = (int8_t)ref[l];
        }
    }
    for (int l = 0; l < 2; l++)
        if ((mv.pred & (1 << l)) && (mv.ref[l] < 0 || mv.ref[l] >= d->sh.nref[l] || !d->refpic[l][mv.ref[l]])) return false;
    store_mv(d, x, y, w, h, &mv);
    hv_inter(d, x, y, w, h, &mv);
    return true;
}

/* ---- the transform tree (7.3.8.8-7.3.8.10) ---- */

static void cu_qp_coded(Hevc *d, Cu *cu) {
    HvCabac *c = &d->cc;
    int v = 0;
    if (dec(c, &d->ctx[C_QP])) {
        v = 1;
        while (v < 5 && dec(c, &d->ctx[C_QP + 1])) v++;
        if (v == 5) v += eg(c, 0);
    }
    if (v && byp(c)) v = -v;
    /* its range (7.4.9.14): -(26 + QpBdOffsetY / 2) .. 25 + QpBdOffsetY / 2 - a damaged stream's further out would take
       the QP below zero (the modulo of a negative); FFmpeg drops such a slice, this keeps the nearest one allowed */
    int lim = 26 + 3 * (d->s->depth - 8);
    v = CLIP3(-lim, lim - 1, v);
    d->qp_coded = true;
    d->cu_qp_delta = v;
    cu_qp(d, cu);
}

static bool transform_unit(Hevc *d, Cu *cu, int x0, int y0, int xb, int yb, int log2, int depth, int blk,
                           bool cbf_l, bool cbf_cb, bool cbf_cr) {
    if ((cbf_l || cbf_cb || cbf_cr) && d->p->cu_qp_delta && !d->qp_coded) cu_qp_coded(d, cu);
    int ipm = cu->pred == MODE_INTRA ? d->ipm[AT4(d, x0, y0)] : 0;
    if (cu->pred == MODE_INTRA) hv_intra(d, x0, y0, log2, 0, ipm);
    if (cbf_l) {
        for (int j = 0; j < (1 << log2); j += 4)
            for (int i = 0; i < (1 << log2); i += 4) d->flags4[AT4(d, x0 + i, y0 + j)] |= F4_CBF;
        if (!residual(d, cu, x0, y0, log2, 0, ipm)) return false;
    }
    if (log2 > 2 || blk == 3) {
        int xc = (log2 > 2 ? x0 : xb) >> 1, yc = (log2 > 2 ? y0 : yb) >> 1, l2 = log2 > 2 ? log2 - 1 : 2;
        for (int k = 1; k < 3; k++) {
            if (cu->pred == MODE_INTRA) hv_intra(d, xc, yc, l2, k, cu->ipm_c);
            if ((k == 1 ? cbf_cb : cbf_cr) && !residual(d, cu, xc, yc, l2, k, cu->ipm_c)) return false;
        }
    }
    (void)depth;
    edges(d, x0, y0, 1 << log2, cu->pred != MODE_INTRA);
    return true;
}

static bool transform_tree(Hevc *d, Cu *cu, int x0, int y0, int xb, int yb, int log2, int depth, int blk,
                           bool pcb, bool pcr) {
    HvCabac *c = &d->cc;
    const HvSps *s = d->s;
    bool split;
    if (log2 <= s->log2_max_tb && log2 > s->log2_min_tb && depth < cu->max_depth && !(cu->intra_split && depth == 0))
        split = dec(c, &d->ctx[C_SPLIT_TF + 5 - log2]);
    else
        split = log2 > s->log2_max_tb || (cu->intra_split && depth == 0) ||
                (s->depth_inter == 0 && cu->pred == MODE_INTER && cu->part != PART_2Nx2N && depth == 0);
    bool cb = false, cr = false;
    if (log2 > 2) {
        if (depth == 0 || pcb) cb = dec(c, &d->ctx[C_CBF_CHROMA + depth]);
        if (depth == 0 || pcr) cr = dec(c, &d->ctx[C_CBF_CHROMA + depth]);
    } else { /* 4x4 luma: the chroma of the 8x8 above it, sent with the last of the four */
        cb = pcb, cr = pcr;
    }
    if (split) {
        int h = 1 << (log2 - 1);
        for (int i = 0; i < 4; i++)
            if (!transform_tree(d, cu, x0 + (i & 1) * h, y0 + (i >> 1) * h, x0, y0, log2 - 1, depth + 1, i, cb, cr))
                return false;
        return true;
    }
    bool cbf_l = true;
    if (cu->pred == MODE_INTRA || depth != 0 || cb || cr) cbf_l = dec(c, &d->ctx[C_CBF_LUMA + (depth == 0 ? 1 : 0)]);
    return transform_unit(d, cu, x0, y0, xb, yb, log2, depth, blk, cbf_l, cb, cr);
}

/* ---- PCM (7.3.8.7) ---- */

static bool pcm_sample(Hevc *d, Cu *cu) {
    const HvSps *s = d->s;
    const uint8_t *p = cabac_after(&d->cc);
    int n = cu->size, nc = n >> 1;
    size_t bits = (size_t)n * n * s->pcm_depth + 2 * (size_t)nc * nc * s->pcm_depth_c;
    if (p + (bits + 7) / 8 > d->cc.end) return false;
    size_t pos = 0;
    for (int k = 0; k < 3; k++) {
        int sz = k ? nc : n, bd = k ? s->pcm_depth_c : s->pcm_depth, x0 = k ? cu->x0 >> 1 : cu->x0, y0 = k ? cu->y0 >> 1 : cu->y0;
        px *dst = d->cur->plane[k] + (size_t)y0 * d->cur->stride[k] + x0;
        for (int y = 0; y < sz; y++)
            for (int x = 0; x < sz; x++) {
                uint32_t v = 0;
                for (int i = 0; i < bd; i++, pos++) v = (v << 1) | ((p[pos >> 3] >> (7 - (pos & 7))) & 1);
                dst[(size_t)y * d->cur->stride[k] + x] = (px)(v << (s->depth - bd));
            }
    }
    hv_cabac_start(&d->cc, p + (bits + 7) / 8, d->cc.end);
    return true;
}

/* ---- coding units (7.3.8.5) ---- */

static bool coding_unit(Hevc *d, int x0, int y0, int log2, int depth) {
    HvCabac *c = &d->cc;
    const HvSps *s = d->s;
    const HvPps *p = d->p;
    Cu cu = {0};
    cu.x0 = x0, cu.y0 = y0, cu.log2 = log2, cu.size = 1 << log2, cu.depth = depth;
    if (p->bypass) cu.bypass = dec(c, &d->ctx[C_BYPASS]);
    bool skip = false;
    if (d->sh.type != SLICE_I) {
        int inc = (hv_avail(d, x0, y0, x0 - 1, y0) && d->skipf[MINCB(d, x0 - 1, y0)]) +
                  (hv_avail(d, x0, y0, x0, y0 - 1) && d->skipf[MINCB(d, x0, y0 - 1)]);
        skip = dec(c, &d->ctx[C_SKIP + inc]);
    }
    if (d->qg_new) { /* the quantization group's predicted QP (8.6.1) */
        int qa = (d->qg_x & (d->ctb - 1)) ? d->qp4[AT4(d, d->qg_x - 1, d->qg_y)] : d->qp_y;
        int qb = (d->qg_y & (d->ctb - 1)) ? d->qp4[AT4(d, d->qg_x, d->qg_y - 1)] : d->qp_y;
        d->qg_pred = (qa + qb + 1) >> 1;
        d->qg_new = false;
    }
    cu_qp(d, &cu);
    int n = cu.size;
    for (int j = 0; j < n; j += (1 << s->log2_min_cb))
        for (int i = 0; i < n; i += (1 << s->log2_min_cb)) {
            d->ctdepth[MINCB(d, x0 + i, y0 + j)] = (uint8_t)depth;
            d->skipf[MINCB(d, x0 + i, y0 + j)] = (uint8_t)skip;
        }
    uint8_t f4 = cu.bypass ? (F4_NOFILTER | F4_BYPASS) : 0;
    for (int j = 0; j < n; j += 4)
        for (int i = 0; i < n; i += 4) d->flags4[AT4(d, x0 + i, y0 + j)] = f4;
    bool ok = true;
    if (skip) {
        cu.pred = MODE_SKIP, cu.part = PART_2Nx2N;
        ok = prediction_unit(d, &cu, x0, y0, n, n, 0);
        if (ok) edges(d, x0, y0, n, true);
    } else {
        cu.pred = d->sh.type == SLICE_I ? MODE_INTRA : dec(c, &d->ctx[C_PRED]) ? MODE_INTRA : MODE_INTER;
        cu.part = PART_2Nx2N;
        if (cu.pred != MODE_INTRA || log2 == s->log2_min_cb) {
            if (dec(c, &d->ctx[C_PART + 0])) cu.part = PART_2Nx2N;
            else if (cu.pred == MODE_INTRA) cu.part = PART_NxN;
            else if (log2 == s->log2_min_cb) {
                if (dec(c, &d->ctx[C_PART + 1])) cu.part = PART_2NxN;
                else if (log2 == 3) cu.part = PART_Nx2N;
                else cu.part = dec(c, &d->ctx[C_PART + 2]) ? PART_Nx2N : PART_NxN;
            } else if (!s->amp) {
                cu.part = dec(c, &d->ctx[C_PART + 1]) ? PART_2NxN : PART_Nx2N;
            } else if (dec(c, &d->ctx[C_PART + 1])) {
                cu.part = dec(c, &d->ctx[C_PART + 3]) ? PART_2NxN : byp(c) ? PART_2NxnD : PART_2NxnU;
            } else {
                cu.part = dec(c, &d->ctx[C_PART + 3]) ? PART_Nx2N : byp(c) ? PART_nRx2N : PART_nLx2N;
            }
        }
        if (cu.pred == MODE_INTRA) {
            HvMv none = {{{0, 0}, {0, 0}}, {-1, -1}, 0, 0};
            store_mv(d, x0, y0, n, n, &none);
            if (cu.part == PART_2Nx2N && s->pcm && log2 >= s->log2_min_pcm && log2 <= s->log2_max_pcm)
                cu.pcm = term(c);
            if (cu.pcm) {
                uint8_t pf = F4_PCM | (s->pcm_nofilter ? F4_NOFILTER : 0);
                for (int j = 0; j < n; j += 4)
                    for (int i = 0; i < n; i += 4) d->flags4[AT4(d, x0 + i, y0 + j)] |= pf;
                set_ipm(d, x0, y0, n, 1);
                ok = pcm_sample(d, &cu);
                if (ok) edges(d, x0, y0, n, false);
            } else {
                int nb = cu.part == PART_NxN ? 4 : 1, off = cu.part == PART_NxN ? n / 2 : n;
                int prev[4];
                for (int i = 0; i < nb; i++) prev[i] = dec(c, &d->ctx[C_PREV_INTRA]);
                for (int i = 0; i < nb; i++) {
                    int xp = x0 + (i & 1) * off, yp = y0 + (i >> 1) * off;
                    int a = cand_mode(d, xp, yp, xp - 1, yp, false), b = cand_mode(d, xp, yp, xp, yp - 1, true);
                    int list[3];
                    if (a == b) {
                        if (a < 2) list[0] = 0, list[1] = 1, list[2] = 26;
                        else list[0] = a, list[1] = 2 + ((a + 29) % 32), list[2] = 2 + ((a - 2 + 1) % 32);
                    } else {
                        list[0] = a, list[1] = b;
                        list[2] = (a && b) ? 0 : (a != 1 && b != 1) ? 1 : 26;
                    }
                    int mode;
                    if (prev[i]) {
                        int idx = byp(c) ? (byp(c) ? 2 : 1) : 0;
                        mode = list[idx];
                    } else {
                        mode = (int)bypn(c, 5);
                        if (list[0] > list[1]) { int t = list[0]; list[0] = list[1]; list[1] = t; }
                        if (list[0] > list[2]) { int t = list[0]; list[0] = list[2]; list[2] = t; }
                        if (list[1] > list[2]) { int t = list[1]; list[1] = list[2]; list[2] = t; }
                        for (int k = 0; k < 3; k++)
                            if (mode >= list[k]) mode++;
                    }
                    cu.ipm[i] = mode;
                    set_ipm(d, xp, yp, off, mode);
                }
                int cm = 4;
                if (dec(c, &d->ctx[C_CHROMA])) cm = (int)bypn(c, 2);
                static const int CM[4] = {0, 26, 10, 1};
                cu.ipm_c = cm == 4 ? cu.ipm[0] : CM[cm] == cu.ipm[0] ? 34 : CM[cm];
            }
        } else {
            int h = n / 2, q = n / 4;
            switch (cu.part) {
            case PART_2Nx2N: ok = prediction_unit(d, &cu, x0, y0, n, n, 0); break;
            case PART_2NxN: ok = prediction_unit(d, &cu, x0, y0, n, h, 0) && prediction_unit(d, &cu, x0, y0 + h, n, h, 1); break;
            case PART_Nx2N: ok = prediction_unit(d, &cu, x0, y0, h, n, 0) && prediction_unit(d, &cu, x0 + h, y0, h, n, 1); break;
            case PART_2NxnU: ok = prediction_unit(d, &cu, x0, y0, n, q, 0) && prediction_unit(d, &cu, x0, y0 + q, n, n - q, 1); break;
            case PART_2NxnD: ok = prediction_unit(d, &cu, x0, y0, n, n - q, 0) && prediction_unit(d, &cu, x0, y0 + n - q, n, q, 1); break;
            case PART_nLx2N: ok = prediction_unit(d, &cu, x0, y0, q, n, 0) && prediction_unit(d, &cu, x0 + q, y0, n - q, n, 1); break;
            case PART_nRx2N: ok = prediction_unit(d, &cu, x0, y0, n - q, n, 0) && prediction_unit(d, &cu, x0 + n - q, y0, q, n, 1); break;
            default:
                ok = prediction_unit(d, &cu, x0, y0, h, h, 0) && prediction_unit(d, &cu, x0 + h, y0, h, h, 1) &&
                     prediction_unit(d, &cu, x0, y0 + h, h, h, 2) && prediction_unit(d, &cu, x0 + h, y0 + h, h, h, 3);
            }
        }
        if (ok && !cu.pcm) {
            bool root = true;
            if (cu.pred != MODE_INTRA && !(cu.part == PART_2Nx2N && cu.merge0)) root = dec(c, &d->ctx[C_ROOT_CBF]);
            if (root) {
                cu.intra_split = cu.pred == MODE_INTRA && cu.part == PART_NxN;
                cu.max_depth = cu.pred == MODE_INTRA ? s->depth_intra + cu.intra_split : s->depth_inter;
                ok = transform_tree(d, &cu, x0, y0, x0, y0, log2, 0, 0, false, false);
            } else {
                edges(d, x0, y0, n, true);
            }
        }
    }
    for (int j = 0; j < n; j += 4)
        for (int i = 0; i < n; i += 4) d->qp4[AT4(d, x0 + i, y0 + j)] = (int8_t)d->qp_y;
    return ok && !overread(c);
}

static bool quadtree(Hevc *d, int x0, int y0, int log2, int depth) {
    const HvSps *s = d->s;
    int n = 1 << log2;
    bool split;
    if (x0 + n <= s->width && y0 + n <= s->height && log2 > s->log2_min_cb) {
        int inc = (hv_avail(d, x0, y0, x0 - 1, y0) && d->ctdepth[MINCB(d, x0 - 1, y0)] > depth) +
                  (hv_avail(d, x0, y0, x0, y0 - 1) && d->ctdepth[MINCB(d, x0, y0 - 1)] > depth);
        split = dec(&d->cc, &d->ctx[C_SPLIT_CU + inc]);
    } else {
        split = log2 > s->log2_min_cb;
    }
    if (log2 >= s->log2_ctb - d->p->diff_qp_depth) {
        d->qp_coded = false, d->cu_qp_delta = 0;
        d->qg_new = true, d->qg_x = x0, d->qg_y = y0;
    }
    if (!split) return coding_unit(d, x0, y0, log2, depth);
    int h = n >> 1;
    if (!quadtree(d, x0, y0, log2 - 1, depth + 1)) return false;
    if (x0 + h < s->width && !quadtree(d, x0 + h, y0, log2 - 1, depth + 1)) return false;
    if (y0 + h < s->height && !quadtree(d, x0, y0 + h, log2 - 1, depth + 1)) return false;
    if (x0 + h < s->width && y0 + h < s->height && !quadtree(d, x0 + h, y0 + h, log2 - 1, depth + 1)) return false;
    return true;
}

/* ---- slice data (7.3.8.1) ---- */

bool hv_slice_data(Hevc *d, const uint8_t *data, size_t n) {
    const HvPps *p = d->p;
    const HvSlice *sh = &d->sh;
    int ts = d->rs2ts[sh->address];
    hv_cabac_start(&d->cc, data, data + n);
    bool first = true;
    for (;;) {
        if (ts >= d->nctb) return false;
        int rs = d->ts2rs[ts], cx = rs % d->wctb, cy = rs / d->wctb;
        int x0 = cx << d->s->log2_ctb, y0 = cy << d->s->log2_ctb;
        bool tile_start = ts == 0 || d->tile_id[ts] != d->tile_id[ts - 1];
        bool row_start = p->wpp && cx == d->col_bd[d->col_of[rs]];
        d->ctb_addr[rs] = (int16_t)d->slice_addr_rs;
        d->ctb_info[rs] = (int16_t)(d->ninfo - 1);
        d->cur->ctb_slice[rs] = (int16_t)d->slice_no;
        if (tile_start && (first || p->tiles)) {
            hv_ctx_init(d);
        } else if (row_start) {
            if (hv_avail(d, x0, y0, x0 + d->ctb, y0 - d->ctb)) memcpy(d->ctx, d->ctx_wpp, sizeof d->ctx);
            else hv_ctx_init(d);
        } else if (first) {
            if (sh->dependent && d->have_dep) memcpy(d->ctx, d->ctx_dep, sizeof d->ctx);
            else hv_ctx_init(d);
        }
        if ((first && !sh->dependent) || tile_start || row_start) d->qp_y = sh->qp;
        first = false;
        if (sh->sao_luma || sh->sao_chroma) sao_syntax(d, cx, cy, rs);
        if (!quadtree(d, x0, y0, d->s->log2_ctb, 0)) return false;
        if (p->wpp && cx == d->col_bd[d->col_of[rs]] + 1) memcpy(d->ctx_wpp, d->ctx, sizeof d->ctx);
        int end = term(&d->cc);
        ts++;
        if (end) {
            if (p->dep_slices) memcpy(d->ctx_dep, d->ctx, sizeof d->ctx), d->have_dep = true;
            return true;
        }
        if (ts >= d->nctb) return false;
        int nrs = d->ts2rs[ts];
        if ((p->tiles && d->tile_id[ts] != d->tile_id[ts - 1]) || (p->wpp && nrs % d->wctb == d->col_bd[d->col_of[nrs]])) {
            if (!term(&d->cc)) return false;
            const uint8_t *next = cabac_after(&d->cc);
            if (next >= d->cc.end) return false;
            hv_cabac_start(&d->cc, next, d->cc.end);
        }
    }
}
