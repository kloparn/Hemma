/* modules/av/hevc_int.h — the HEVC decoder's insides, shared by hevc.c (parameter sets, slices, pictures),
   hevc_ctu.c (CABAC and the coding tree) and hevc_dsp.c (transforms, prediction, loop filters). Not an API. */
#ifndef HEMMA_AV_HEVC_INT_H
#define HEMMA_AV_HEVC_INT_H

#include "hevc.h"

#include <stdlib.h>
#include <string.h>

#define HV_MAX_SPS 16
#define HV_MAX_PPS 64
#define HV_MAX_DPB 18       /* sps_max_dec_pic_buffering (16 at most) + the picture being decoded + one handed out */
#define HV_MAX_REF 16
#define HV_MAX_RPS 64       /* short-term reference picture sets in an SPS (+1 in a slice header) */
#define HV_MAX_LT 32
#define HV_MAX_TILE 32      /* tile columns / rows (level 6.2 allows 20 x 22) */
#define HV_PAD 80           /* luma samples around each picture for motion compensation (chroma: half) */
#define HV_NCTX 154

#ifdef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#define HV_MAX_PIXELS (720 * 576) /* the fuzzer's pictures stay small: thousands of inputs a second */
#else
#define HV_MAX_PIXELS (8192 * 4352)
#endif

#define CLIP3(lo, hi, v) ((v) < (lo) ? (lo) : (v) > (hi) ? (hi) : (v))

typedef uint16_t px;

enum { NAL_TRAIL_N, NAL_TRAIL_R, NAL_TSA_N, NAL_TSA_R, NAL_STSA_N, NAL_STSA_R, NAL_RADL_N, NAL_RADL_R, NAL_RASL_N,
       NAL_RASL_R, NAL_BLA_W_LP = 16, NAL_BLA_W_RADL, NAL_BLA_N_LP, NAL_IDR_W_RADL, NAL_IDR_N_LP, NAL_CRA,
       NAL_VPS = 32, NAL_SPS, NAL_PPS, NAL_AUD, NAL_EOS, NAL_EOB, NAL_FD, NAL_SEI_PREFIX, NAL_SEI_SUFFIX };
enum { SLICE_B, SLICE_P, SLICE_I };
enum { PART_2Nx2N, PART_2NxN, PART_Nx2N, PART_NxN, PART_2NxnU, PART_2NxnD, PART_nLx2N, PART_nRx2N };

typedef struct {
    int nneg, npos;          /* negatives first in delta[] (closest first), then positives */
    int32_t delta[HV_MAX_REF];
    uint8_t used[HV_MAX_REF];
} HvRps;

typedef struct {
    uint8_t list[4][6][64];  /* coefficients in up-right diagonal order (4x4: 16) */
    uint8_t dc[4][6];        /* 16x16 and 32x32: the DC value */
} HvScaling;

typedef struct {
    bool valid;
    const char *unsup;
    int chroma, width, height;
    int crop_l, crop_r, crop_t, crop_b;          /* luma samples */
    int depth, depth_c;
    int log2_poc_lsb;
    int max_dec, max_reorder, max_latency;       /* the highest sub-layer's */
    int log2_min_cb, log2_ctb, log2_min_tb, log2_max_tb;
    int depth_inter, depth_intra;
    bool scaling; HvScaling sl;
    bool amp, sao, pcm, pcm_nofilter, tmvp, strong;
    int pcm_depth, pcm_depth_c, log2_min_pcm, log2_max_pcm;
    int nrps; HvRps rps[HV_MAX_RPS];
    bool lt_present; int nlt_sps; int lt_lsb_sps[HV_MAX_LT]; uint8_t lt_used_sps[HV_MAX_LT];
    int sar_num, sar_den, rate_num, rate_den;
} HvSps;

typedef struct {
    bool valid;
    const char *unsup;
    int sps;
    bool dep_slices, output_flag, sign_hiding, cabac_init_present, constrained_intra, transform_skip;
    bool cu_qp_delta, slice_chroma_offsets, weighted_p, weighted_b, bypass, tiles, wpp, uniform;
    bool lf_tiles, lf_slices, deblock_override, deblock_off, scaling, lists_mod, hdr_ext;
    int extra_bits, nref[2], init_qp, diff_qp_depth, cb_off, cr_off;
    int ncols, nrows, col_w[HV_MAX_TILE], row_h[HV_MAX_TILE];
    int beta, tc;                     /* offsets, already doubled */
    HvScaling sl;
    int log2_merge;
} HvPps;

/* one 4x4 block's motion: pred 0 = intra (or nothing yet), bit 0 = list 0, bit 1 = list 1 */
typedef struct {
    int16_t mv[2][2];
    int8_t ref[2];
    uint8_t pred;
    uint8_t pad;
} HvMv;

typedef struct {
    int poc[2][HV_MAX_REF];
    uint8_t lt[2][HV_MAX_REF];
    int n[2];
} HvRefs;

typedef struct {
    bool busy;                 /* holds a picture: referenced, waiting for output, or being decoded */
    px *base[3], *plane[3];
    int stride[3];
    int poc;
    int ref;                   /* 0 unused, 1 short-term, 2 long-term */
    bool output, handed;       /* waiting for output / handed out by hevc_frame (kept until the next call) */
    bool missing;              /* made up for a reference that wasn't there */
    int64_t pts;
    int damaged;
    char type;
    int seq;                   /* which coded video sequence (output never mixes two) */
    HvMv *mv;                  /* 4x4 grid */
    int16_t *ctb_slice;        /* per CTB: its slice's index in refs[] (-1 none) */
    HvRefs *refs;
    int nrefs, crefs;
    int w, h, cw, ch;          /* the allocation's sizes (luma, chroma) */
    int nctb;
    int crop_l, crop_r, crop_t, crop_b, depth, sar_num, sar_den, rate_num, rate_den; /* its SPS's */
} HvPic;

typedef struct {               /* what the loop filters need of a slice */
    int addr_rs;               /* SliceAddrRs: the first CTB of the (independent) slice */
    bool deblock_off, lf_slices, sao_luma, sao_chroma;
    int beta, tc;
} HvSliceInfo;

typedef struct {
    uint8_t type[3], band[3], eo[3];
    int8_t off[3][5];
} HvSao;

typedef struct {
    int first, no_output_prior, pps, dependent, address;
    int type, output, poc_lsb;
    HvRps rps_own; const HvRps *rps; bool have_rps;
    int nlt_sps, nlt; int lt_lsb[HV_MAX_LT]; uint8_t lt_used[HV_MAX_LT], lt_msb[HV_MAX_LT]; int lt_cycle[HV_MAX_LT];
    bool tmvp, sao_luma, sao_chroma, mvd_l1_zero, cabac_init, col_l0, deblock_off, lf_slices;
    int nref[2]; bool mod[2]; int entry[2][HV_MAX_REF];
    int col_ref, max_merge, qp, cb_off, cr_off, beta, tc;
    int lw_denom, cw_denom;
    int16_t lw[2][HV_MAX_REF], lo[2][HV_MAX_REF], cw[2][HV_MAX_REF][2], co[2][HV_MAX_REF][2];
    bool wflag[2][HV_MAX_REF];  /* any weight given for that reference (a plain average otherwise) */
    size_t data;               /* byte where slice data starts */
} HvSlice;

typedef struct {
    const uint8_t *p, *end;
    uint32_t range, value;
    int cnt;                   /* lookahead bits held below the 9-bit offset in value */
    const uint8_t *start;
} HvCabac;

struct Hevc {
    HvSps *sps[HV_MAX_SPS];
    HvPps *pps[HV_MAX_PPS];
    int nal_len;               /* hvcC's NAL length size, 0 = Annex B */
    const char *unsup;
    /* the active sequence and picture parameter sets, and what's derived from them */
    const HvSps *s; const HvPps *p;
    int act_sps, act_pps;      /* ids, -1 none */
    int ctb, wctb, hctb, nctb, wmin, hmin, w4, h4;
    int *rs2ts, *ts2rs, *tile_id, *col_of, *row_of; /* per CTB; col_of/row_of: its tile's column/row */
    int col_bd[HV_MAX_TILE + 1], row_bd[HV_MAX_TILE + 1];
    int *zs; int zw;           /* MinTbAddrZs */
    uint8_t *ctdepth, *skipf;  /* per min CB */
    uint8_t *ipm, *flags4;     /* per 4x4: intra mode; F4_* */
    int8_t *qp4;
    uint8_t *bs_v, *bs_h;      /* boundary strength: vertical edges per 8 x 4, horizontal per 4 x 8 */
    int16_t *ctb_addr;         /* per CTB: SliceAddrRs of its slice (-1 not decoded) */
    int16_t *ctb_info;         /* per CTB: index into info[] */
    HvSao *sao;                /* per CTB */
    HvSliceInfo *info; int ninfo, cinfo;
    int16_t scale[4][6][1024]; /* scaling factors m[x][y] (row-major), from the active lists */
    bool scale_flat;
    px *tmp; size_t ntmp;      /* SAO's copy of the deblocked picture */
    /* pictures */
    HvPic pics[HV_MAX_DPB];
    HvPic *cur;
    int seq;
    bool first_pic, no_rasl, wait_irap, eos;
    int prev_tid0_poc;
    int poc;
    int nal_type, temporal_id;
    int last_type;
    /* the slice being decoded */
    HvSlice sh;
    int slice_addr_rs;
    HvPic *refpic[2][HV_MAX_REF];
    bool lowdelay;             /* NoBackwardPredFlag */
    int slice_no;              /* this slice's index in cur->refs */
    /* entropy decoding */
    HvCabac cc;
    uint8_t ctx[HV_NCTX], ctx_wpp[HV_NCTX], ctx_dep[HV_NCTX];
    bool have_dep;
    int qp_y, cu_qp_delta, qg_x, qg_y, qg_pred;
    bool qp_coded, qg_new;
    int16_t coeff[32 * 32];
    /* output */
    HvPic *out_fifo[HV_MAX_DPB]; int nout;
    HvPic *handed;
    uint8_t *out8; size_t nout8;
    /* the rbsp being read */
    uint8_t *rbsp; size_t crbsp;
    uint8_t *annexb; size_t nannexb, cannexb, apos; /* Annex B input; apos: where reading goes on */
    bool flushing;
    int64_t pts;
    bool au_mode;
};

enum { F4_NOFILTER = 1, F4_CBF = 2, F4_PCM = 4, F4_BYPASS = 8 };

/* hevc_ctu.c */
extern const uint8_t hv_lps[64][4], hv_next_lps[64]; /* CABAC's rangeTabLps and transIdxLps (9.3.4.3.2) */
extern const uint8_t hv_init[3][HV_NCTX];            /* every context's initValue by initType (also the tests' writer) */
enum { /* the contexts' places in hv_init (one per ctxInc) */
    C_SAO_MERGE = 0, C_SAO_TYPE = 1, C_SPLIT_CU = 2, C_BYPASS = 5, C_SKIP = 6, C_QP = 9, C_PRED = 11, C_PART = 12,
    C_PREV_INTRA = 16, C_CHROMA = 17, C_MERGE_FLAG = 18, C_MERGE_IDX = 19, C_INTER_DIR = 20, C_REF_IDX = 25,
    C_MVD_G0 = 27, C_MVD_G1 = 28, C_MVP = 29, C_ROOT_CBF = 30, C_SPLIT_TF = 31, C_CBF_LUMA = 34, C_CBF_CHROMA = 36,
    C_TSKIP = 40, C_LAST_X = 42, C_LAST_Y = 60, C_CSBF = 78, C_SIG = 82, C_G1 = 124, C_G2 = 148
};
void hv_cabac_start(HvCabac *c, const uint8_t *p, const uint8_t *end);
void hv_ctx_init(Hevc *d);
bool hv_slice_data(Hevc *d, const uint8_t *data, size_t n); /* false: it went wrong somewhere (the rest left out) */

/* hevc_dsp.c */
void hv_transform(int32_t *res, const int16_t *coeff, int log2, bool dst, int depth);
void hv_transform_dc(int32_t *res, int dc, int log2, int depth);
void hv_intra(Hevc *d, int x0, int y0, int log2, int c, int mode);
void hv_inter(Hevc *d, int x, int y, int w, int h, const HvMv *mv);
void hv_deblock(Hevc *d);
void hv_sao(Hevc *d);
bool hv_avail(const Hevc *d, int xc, int yc, int xn, int yn);

#endif
