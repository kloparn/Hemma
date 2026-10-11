/* modules/av/h264enc.h — H.264 video encoder (Constrained Baseline profile).
 *
 * Turns 8-bit 4:2:0 frames into H.264 that every browser, phone and TV plays
 * with hardware decoding: I and P frames only (no B frames, no CABAC, no
 * interlace), one reference frame, CAVLC, in-loop deblocking. Intra 16x16,
 * intra 4x4 and chroma prediction; P_Skip and P 16x16 / 16x8 / 8x16 with
 * quarter-pel motion search. Constant quality (a QP per frame type, lowered
 * a little for I frames) with an optional bitrate cap.
 *
 * The reconstruction it keeps (h264enc_recon) is exactly what any decoder
 * shows: H.264 decoding is bit-exact by specification, which is how this
 * encoder is checked against a browser's decoder.
 *
 * Feed one picture, get one packet back (no B frames, so no reordering and
 * no delay). Packets are Annex-B (start codes, SPS/PPS before every IDR) for
 * raw .h264 files and WebCodecs, or 4-byte length-prefixed for MP4 with
 * h264enc_avcc() as the 'avcC' box payload.
 *
 * Pure library: libc only (no core/), no mutable globals, no OS calls — one
 * encoder per thread, any number of them. Builds for x86-64, aarch64/armv6
 * and wasm32. */
#ifndef HEMMA_AV_H264ENC_H
#define HEMMA_AV_H264ENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    H264_FAST = 0,   /* 16x16 motion search, intra 4x4 in I frames only */
    H264_MEDIUM = 1, /* + 16x8/8x16 partitions, intra 4x4 everywhere, wider search */
} H264Preset;

typedef struct {
    int width, height;    /* picture size; even, 16..4096 (coded as whole macroblocks + cropping) */
    int fps_num, fps_den; /* frame rate, e.g. 30000/1001 (0/0 = 25) */
    int sar_w, sar_h;     /* pixel aspect ratio (0/0 = square). DVD NTSC 4:3 = 8:9, 16:9 = 32:27 */
    int quality;          /* QP of P frames, 1..51 like x264's CRF: lower = better and bigger; 0 = 23 */
    int keyint;           /* frames between IDR frames (seek points); 0 = about 2 s */
    int max_kbps;         /* bitrate cap (0 = none): QP rises when the stream runs above it */
    H264Preset preset;
    bool annexb;          /* true: start codes + SPS/PPS in front of every IDR; false: 4-byte lengths */
    int colour;           /* 601 or 709 (BT.601 / BT.709 colour description); 0 = 709 from 720 lines up */
} H264Params;

typedef struct {
    const uint8_t *y, *u, *v; /* 4:2:0 planes: chroma is half width and half height (rounded up) */
    int stride_y, stride_u, stride_v;
    int64_t pts; /* passed through to the packet */
} H264Picture;

typedef struct {
    const uint8_t *data; /* the frame's NAL units; valid until the next encode / free */
    size_t size;
    bool idr;          /* IDR frame: a sync sample / seek point */
    int64_t pts;
    int qp;            /* QP the frame was coded with */
    int frame;         /* 0, 1, 2, ... */
} H264Packet;

typedef struct H264Enc H264Enc;

H264Enc *h264enc_new(const H264Params *p); /* NULL on bad parameters or out of memory */
void h264enc_free(H264Enc *e);

/* Encode one picture. False only on out-of-memory. */
bool h264enc_encode(H264Enc *e, const H264Picture *pic, H264Packet *out);

/* The parameter sets as NAL units: header byte first, no start code / length. */
const uint8_t *h264enc_sps(const H264Enc *e, size_t *n);
const uint8_t *h264enc_pps(const H264Enc *e, size_t *n);
/* AVCDecoderConfigurationRecord (MP4 'avcC' payload, WebCodecs description).
   Returns its size, 0 when cap is too small. */
size_t h264enc_avcc(const H264Enc *e, uint8_t *out, size_t cap);
/* Codec string for MIME types and WebCodecs, e.g. "avc1.42C01E". */
void h264enc_codec(const H264Enc *e, char out[16]);
/* The decoded picture of the last encoded frame (what every decoder shows),
   at the picture size given in H264Params. Valid until the next encode. */
void h264enc_recon(const H264Enc *e, const uint8_t **y, const uint8_t **u, const uint8_t **v, int *stride_y,
                   int *stride_c);

#endif
