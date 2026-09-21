// SDK2 THP: real Motion-JPEG video decoder for Melee FMVs.
//
// THP video frames are self-contained baseline-JPEG streams (SOI ...
// EOI, with per-frame DQT/DHT). This file implements a compact baseline
// decoder (Huffman, dequant, IDCT, YCbCr->RGB) and converts to the YUV420
// planes lbmthp uploads as I8 textures (Y full-res, U/V quarter-res).
//
// Design notes:
// - No dependency on the SDK THPFileInfo layout: decoded frames live in
//   a small side cache keyed by the work pointer; THPDec_80331340/D0
//   copy them into the game planes. A corrupt frame keeps the previous
//   one (freeze) instead of hanging the player.
// - Sampling factors 4:2:0, 4:2:2 and 4:4:4 are supported; anything else
//   (progressive, arithmetic, 12-bit) fails cleanly to a black frame.
// - lbmthp is video-only (no movie-audio path exists in the game), so no
//   audio handling is needed here.

#include <dolphin/thp/thp.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ------------------------------------------------------------ bit reader
typedef struct {
    const u8* p;
    const u8* end;
    u32 buf;
    int nbits;
    int rst_found; // restart marker number seen (0-7), or -1
    int eoi;       // EOI seen
} BitRd;

static void br_fill(BitRd* b)
{
    while (b->nbits <= 24 && b->p < b->end && !b->eoi && b->rst_found < 0) {
        unsigned c = *b->p++;
        if (c == 0xFF) {
            unsigned m = (b->p < b->end) ? *b->p++ : 0xD9;
            if (m == 0x00) {
                c = 0xFF; // stuffed byte
            } else if (m >= 0xD0 && m <= 0xD7) {
                b->rst_found = (int) (m - 0xD0);
                --b->p; // rewind: outer loop consumes the marker
                return;
            } else if (m == 0xD9) {
                b->eoi = 1;
                return;
            } else {
                // unexpected marker inside scan: stop cleanly
                b->eoi = 1;
                return;
            }
        }
        b->buf = (b->buf << 8) | c;
        b->nbits += 8;
    }
}

static unsigned br_bits(BitRd* b, int n)
{
    br_fill(b);
    if (b->nbits < n) {
        // truncated stream: pad with zeros (graceful degradation)
        unsigned v = (b->nbits > 0) ? (b->buf << (n - b->nbits)) : 0;
        b->buf = 0;
        b->nbits = 0;
        return v & (n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1));
    }
    b->nbits -= n;
    return (b->buf >> b->nbits) & (n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1));
}

// ------------------------------------------------------------ huffman
typedef struct {
    u8 sym[256];
    int firstcode[17]; // indexed by length 1..16
    int count[17];
    int symoff[17];
    int nsym;
} HuffTab;

static void huff_build(HuffTab* h, const u8* counts, const u8* symbols,
                       int nsym)
{
    int code = 0, off = 0, len, i;
    memset(h, 0, sizeof(*h));
    for (len = 1; len <= 16; len++) {
        h->count[len] = counts[len - 1];
        h->symoff[len] = off;
        h->firstcode[len] = code;
        for (i = 0; i < h->count[len] && off < 256; i++, off++)
            h->sym[off] = symbols[off];
        code = (code + h->count[len]) << 1;
    }
    h->nsym = off;
}

static int huff_decode(BitRd* b, const HuffTab* h)
{
    int code = 0, len;
    for (len = 1; len <= 16; len++) {
        code = (code << 1) | (int) br_bits(b, 1);
        if (b->eoi && b->nbits == 0 && len > 12) {
            return 0; // truncated: treat as EOB-ish zero
        }
        if (code - h->firstcode[len] < h->count[len])
            return h->sym[h->symoff[len] + code - h->firstcode[len]];
    }
    return 0;
}

static int huff_extend(unsigned v, int s)
{
    if (s == 0)
        return 0;
    if (v < (1u << (s - 1)))
        v += (unsigned) ((-1 << s) + 1);
    return (int) v;
}

// ------------------------------------------------------------ JPEG state
static const u8 s_zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

typedef struct {
    int id;      // component selector
    int h, v;    // sampling factors
    int tq;      // quant table selector
    int dc_pred; // DC predictor
} JComp;

typedef struct {
    const u8* p;
    const u8* end;
    int w, h;
    int ncomp;
    JComp comp[4];
    int maxh, maxv;
    int restart; // restart interval (0 = none)
    int qtbl[4][64]; // natural order
    HuffTab hdc[4], hac[4];
    int valid;
} Jpeg;

// marker helpers (BE u16 lengths include the 2 length bytes)
static unsigned rd_u16(const u8** pp, const u8* end)
{
    const u8* p = *pp;
    unsigned v = 0;
    if (p + 2 <= end) {
        v = ((unsigned) p[0] << 8) | p[1];
        *pp = p + 2;
    } else {
        *pp = end;
    }
    return v;
}

static int jpeg_headers(Jpeg* j, const u8* data, const u8* end)
{
    const u8* p = data;
    memset(j, 0, sizeof(*j));
    if (end - p < 2 || p[0] != 0xFF || p[1] != 0xD8)
        return 0; // not a JPEG frame
    p += 2;
    while (p + 4 <= end) {
        unsigned m, len;
        if (p[0] != 0xFF) {
            p++;
            continue;
        }
        m = p[1];
        p += 2;
        if (m == 0xD9)
            break; // EOI before SOS: nothing to decode
        if (m == 0xDA)
            break; // SOS: header phase done (parsed below)
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7))
            continue; // standalone markers
        len = rd_u16(&p, end);
        if (len < 2 || p + len - 2 > end)
            return 0;
        {
            const u8* seg = p;
            const u8* segend = p + len - 2;
            if (m == 0xDB) { // DQT
                while (seg + 1 <= segend) {
                    unsigned info = *seg++;
                    unsigned tq = info & 0x0F, prec = info >> 4;
                    int i;
                    if (tq > 3)
                        return 0;
                    for (i = 0; i < 64 && seg < segend; i++) {
                        unsigned v;
                        if (prec) {
                            if (seg + 2 > segend)
                                break;
                            v = ((unsigned) seg[0] << 8) | seg[1];
                            seg += 2;
                        } else {
                            v = *seg++;
                        }
                        j->qtbl[tq][s_zigzag[i]] =
                            (int) (v ? v : 1);
                    }
                }
            } else if (m == 0xC4) { // DHT
                while (seg + 17 <= segend) {
                    unsigned info = *seg++;
                    unsigned tc = info >> 4, th = info & 0x0F;
                    int total = 0, i;
                    u8 counts[16];
                    if (th > 3)
                        return 0;
                    for (i = 0; i < 16; i++) {
                        counts[i] = seg[i];
                        total += seg[i];
                    }
                    seg += 16;
                    if (seg + total > segend)
                        return 0;
                    if (tc)
                        huff_build(&j->hac[th], counts, seg, total);
                    else
                        huff_build(&j->hdc[th], counts, seg, total);
                    seg += total;
                }
            } else if (m == 0xC0) { // SOF0 (baseline only)
                unsigned nf, i;
                if (seg + 6 > segend)
                    return 0;
                if (seg[0] != 8)
                    return 0; // 12-bit: unsupported
                j->h = ((unsigned) seg[1] << 8) | seg[2];
                j->w = ((unsigned) seg[3] << 8) | seg[4];
                nf = seg[5];
                seg += 6;
                if (nf < 1 || nf > 4 || seg + nf * 3 > segend)
                    return 0;
                j->ncomp = (int) nf;
                j->maxh = j->maxv = 1;
                for (i = 0; i < nf; i++) {
                    j->comp[i].id = seg[0];
                    j->comp[i].h = (seg[1] >> 4) & 0x0F;
                    j->comp[i].v = seg[1] & 0x0F;
                    j->comp[i].tq = seg[2];
                    if (j->comp[i].h < 1 || j->comp[i].h > 2 ||
                        j->comp[i].v < 1 || j->comp[i].v > 2) {
                        return 0;
                    }
                    if (j->comp[i].h > j->maxh)
                        j->maxh = j->comp[i].h;
                    if (j->comp[i].v > j->maxv)
                        j->maxv = j->comp[i].v;
                    seg += 3;
                }
            } else if (m == 0xDD) { // DRI
                if (seg + 2 <= segend)
                    j->restart = (int) (((unsigned) seg[0] << 8) | seg[1]);
            } else if (m == 0xC2 || m == 0xC1 || m == 0xC3) {
                return 0; // progressive/lossless: unsupported
            }
            // APPn/COM/DNL/etc: skipped
            p = segend;
        }
    }
    j->p = p;
    j->end = end;
    return j->w > 0 && j->h > 0 && j->ncomp > 0;
}

// ------------------------------------------------------------ IDCT
// Direct 2D IDCT with a precomputed cosine table and row/column skipping
// (video blocks are sparse; DC-only blocks collapse to a constant).
static float s_cos_tbl[8][8];
static int s_cos_init;

static void cos_init(void)
{
    int x, u;
    if (s_cos_init)
        return;
    for (x = 0; x < 8; x++) {
        for (u = 0; u < 8; u++) {
            s_cos_tbl[x][u] =
                (float) cos(((2 * x + 1) * u * M_PI) / 16.0);
        }
    }
    s_cos_init = 1;
}

static void idct_block(const float* in, float* out /*64*/)
{
    int x, y, u, v;
    int r0 = 8, r1 = -1, c0 = 8, c1 = -1;
    int y2, x2;
    // tight active window over nonzero coefficients
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            if (in[y * 8 + x] != 0.f) {
                if (y < r0)
                    r0 = y;
                if (y > r1)
                    r1 = y;
                if (x < c0)
                    c0 = x;
                if (x > c1)
                    c1 = x;
            }
        }
    }
    if (r1 < 0) { // all zero
        for (x2 = 0; x2 < 64; x2++)
            out[x2] = 0;
        return;
    }
    if (r0 == 0 && r1 == 0 && c0 == 0 && c1 == 0) {
        // DC-only fast path (very common in flat areas)
        float dc = in[0] * 0.125f;
        for (x2 = 0; x2 < 64; x2++)
            out[x2] = dc;
        return;
    }
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            float s = 0;
            for (v = r0; v <= r1; v++) {
                float cv = (v == 0) ? 0.707106781f : 1.f;
                float sy = 0;
                for (u = c0; u <= c1; u++) {
                    float cu = (u == 0) ? 0.707106781f : 1.f;
                    sy += cu * in[v * 8 + u] * s_cos_tbl[x][u];
                }
                s += cv * sy * s_cos_tbl[y][v];
            }
            out[y * 8 + x] = s * 0.25f;
        }
    }
}

// ------------------------------------------------------------ scan decode
static int decode_block(BitRd* b, const HuffTab* hdc, const HuffTab* hac,
                        const int* qtbl, int* pred, float* out /*64*/)
{
    int s, k = 0, i;
    float coef[64];
    for (i = 0; i < 64; i++)
        coef[i] = 0;
    s = huff_decode(b, hdc);
    {
        int diff = huff_extend(br_bits(b, s), s);
        *pred += diff;
        coef[0] = (float) (*pred * qtbl[0]);
    }
    k = 1;
    while (k < 64) {
        int rs = huff_decode(b, hac);
        int r = (rs >> 4) & 0x0F, sz = rs & 0x0F;
        if (sz == 0) {
            if (r == 15) {
                k += 16; // ZRL
                continue;
            }
            break; // EOB
        }
        k += r;
        if (k >= 64)
            break;
        {
            int v = huff_extend(br_bits(b, sz), sz);
            coef[s_zigzag[k]] =
                (float) (v * qtbl[s_zigzag[k]]);
        }
        k++;
    }
    idct_block(coef, out);
    return 1;
}

// Decode one frame into full-res Y/Cb/Cr float planes (caller frees via
// out_free). Returns 0 on failure.
static int jpeg_decode_frame(const u8* data, const u8* end, float** out_y,
                             float** out_cb, float** out_cr, int* out_w,
                             int* out_h)
{
    Jpeg j;
    BitRd br;
    int sos_ns, i, mcux, mcuy, mcu;
    int comp_map[4]; // scan order -> component index
    int td[4], ta[4];
    float *plane[4];
    int stride;
    if (!jpeg_headers(&j, data, end))
        return 0;
    if (j.w > 2048 || j.h > 2048 || j.w <= 0 || j.h <= 0)
        return 0;
    // SOS header at j.p
    {
        const u8* p = j.p;
        if (p + 2 > end || p[0] != 0xFF || p[1] != 0xDA)
            return 0;
        p += 2;
        {
            unsigned len = rd_u16(&p, end);
            const u8* send;
            if (len < 6 || p + len - 2 > end)
                return 0;
            send = p + len - 2;
            sos_ns = *p++;
            if (sos_ns < 1 || sos_ns > 4 || sos_ns > j.ncomp)
                return 0;
            for (i = 0; i < sos_ns; i++) {
                int cs = *p++, sel = *p++;
                int ci;
                td[i] = (sel >> 4) & 0x0F;
                ta[i] = sel & 0x0F;
                comp_map[i] = -1;
                for (ci = 0; ci < j.ncomp; ci++) {
                    if (j.comp[ci].id == cs)
                        comp_map[i] = ci;
                }
                if (comp_map[i] < 0)
                    return 0;
            }
            // Ss/Se/Ah/Al: baseline spectral selection only
            if (p + 3 > send || p[0] != 0 || p[1] != 63 || p[2] != 0)
                return 0;
            p += 3;
            br.p = p;
            br.end = end;
            br.buf = 0;
            br.nbits = 0;
            br.rst_found = -1;
            br.eoi = 0;
        }
    }
    stride = j.w;
    for (i = 0; i < 4; i++)
        plane[i] = NULL;
    for (i = 0; i < j.ncomp; i++) {
        plane[i] = (float*) malloc(sizeof(float) * (size_t) j.w * j.h);
        if (!plane[i]) {
            int k;
            for (k = 0; k < i; k++)
                free(plane[k]);
            return 0;
        }
    }
    cos_init();
    mcux = (j.w + 8 * j.maxh - 1) / (8 * j.maxh);
    mcuy = (j.h + 8 * j.maxv - 1) / (8 * j.maxv);
    mcu = 0;
    for (i = 0; i < j.ncomp; i++)
        j.comp[i].dc_pred = 0;
    for (i = 0; i < mcuy; i++) {
        int m;
        for (m = 0; m < mcux; m++, mcu++) {
            int s;
            if (j.restart && mcu && (mcu % j.restart) == 0) {
                // consume RSTn marker, reset predictors
                int k;
                br_fill(&br);
                if (br.rst_found < 0) {
                    // stream desync: stop decoding, keep partial frame
                    goto done;
                }
                br.p += 2; // skip FF Dn
                br.rst_found = -1;
                br.buf = 0;
                br.nbits = 0;
                for (k = 0; k < j.ncomp; k++)
                    j.comp[k].dc_pred = 0;
            }
            for (s = 0; s < sos_ns; s++) {
                int ci = comp_map[s];
                int bi, bj;
                for (bj = 0; bj < j.comp[ci].v; bj++) {
                    for (bi = 0; bi < j.comp[ci].h; bi++) {
                        float blk[64];
                        int dx, dy;
                        if (!decode_block(&br, &j.hdc[td[s]],
                                          &j.hac[ta[s]],
                                          j.qtbl[j.comp[ci].tq],
                                          &j.comp[ci].dc_pred, blk)) {
                            goto done;
                        }
                        // replicate up to full-res plane
                        {
                            int rx = j.maxh / j.comp[ci].h;
                            int ry = j.maxv / j.comp[ci].v;
                            int bx = m * j.maxh + bi;
                            int by = i * j.maxv + bj;
                            for (dy = 0; dy < 8; dy++) {
                                for (dx = 0; dx < 8; dx++) {
                                    float v = blk[dy * 8 + dx] + 128.f;
                                    int px, py, ax, ay;
                                    for (ay = 0; ay < ry; ay++) {
                                        for (ax = 0; ax < rx; ax++) {
                                            px = bx * 8 + dx * rx + ax;
                                            py = by * 8 + dy * ry + ay;
                                            if (px < j.w && py < j.h) {
                                                plane[ci][py * stride +
                                                           px] = v;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        if (br.eoi)
                            goto done;
                    }
                }
            }
        }
    }
done:
    // grayscale: replicate luma into chroma-neutral planes
    if (j.ncomp == 1) {
        int p;
        plane[1] = (float*) malloc(sizeof(float) * (size_t) j.w * j.h);
        plane[2] = (float*) malloc(sizeof(float) * (size_t) j.w * j.h);
        if (!plane[1] || !plane[2]) {
            free(plane[1]);
            free(plane[2]);
            free(plane[0]);
            return 0;
        }
        for (p = 0; p < j.w * j.h; p++) {
            plane[1][p] = 128.f;
            plane[2][p] = 128.f;
        }
    }
    *out_y = plane[0];
    *out_cb = plane[1];
    *out_cr = plane[2];
    *out_w = j.w;
    *out_h = j.h;
    return 1;
}

// ------------------------------------------------------------ YUV420 + cache
typedef struct {
    const THPFileInfo* work;
    int w, h;
    u8* y;
    u8* u;
    u8* v;
    int used;
} ThpCacheEnt;

#define THP_CACHE_N 4
static ThpCacheEnt s_thp[THP_CACHE_N];

static ThpCacheEnt* thp_cache_get(const THPFileInfo* work, int w, int h)
{
    int i, free_slot = -1;
    for (i = 0; i < THP_CACHE_N; i++) {
        if (s_thp[i].used && s_thp[i].work == work && s_thp[i].w == w &&
            s_thp[i].h == h) {
            return &s_thp[i];
        }
        if (!s_thp[i].used && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        free_slot = 0; // evict oldest slot (round-robin would need age)
    {
        ThpCacheEnt* e = &s_thp[free_slot];
        size_t ys = (size_t) w * h, uvs = ys / 4;
        free(e->y);
        free(e->u);
        free(e->v);
        e->y = (u8*) malloc(ys ? ys : 1);
        e->u = (u8*) malloc(uvs ? uvs : 1);
        e->v = (u8*) malloc(uvs ? uvs : 1);
        if (!e->y || !e->u || !e->v) {
            free(e->y);
            free(e->u);
            free(e->v);
            memset(e, 0, sizeof(*e));
            return NULL;
        }
        e->used = 1;
        e->work = work;
        e->w = w;
        e->h = h;
        return e;
    }
}

static ThpCacheEnt* thp_cache_find(const THPFileInfo* work)
{
    int i;
    for (i = 0; i < THP_CACHE_N; i++) {
        if (s_thp[i].used && s_thp[i].work == work)
            return &s_thp[i];
    }
    return NULL;
}

static u8 clamp_u8(float v)
{
    int i = (int) (v + 0.5f);
    if (i < 0)
        i = 0;
    if (i > 255)
        i = 255;
    return (u8) i;
}

// YCbCr float planes -> YUV420 (BT.601). RGB staging keeps the 2x2
// chroma averaging exact.
static void rgb_to_yuv420(const float* yf, const float* cbf,
                          const float* crf, int w, int h, u8* Y, u8* U,
                          u8* V)
{
    int x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float yy = yf[y * w + x];
            float cb = cbf[y * w + x] - 128.f;
            float cr = crf[y * w + x] - 128.f;
            float r = yy + 1.402f * cr;
            float g = yy - 0.344136f * cb - 0.714136f * cr;
            float b = yy + 1.772f * cb;
            if (r < 0)
                r = 0;
            if (r > 255)
                r = 255;
            if (g < 0)
                g = 0;
            if (g > 255)
                g = 255;
            if (b < 0)
                b = 0;
            if (b > 255)
                b = 255;
            Y[y * w + x] = clamp_u8(0.299f * r + 0.587f * g + 0.114f * b);
            if ((x & 1) == 0 && (y & 1) == 0) {
                float r2 = 0, g2 = 0, b2 = 0;
                int dx, dy, n = 0;
                // average the 2x2 RGB quad, then convert
                for (dy = 0; dy < 2; dy++) {
                    for (dx = 0; dx < 2; dx++) {
                        int sx = x + dx, sy = y + dy;
                        if (sx < w && sy < h) {
                            float yq = yf[sy * w + sx];
                            float cq = cbf[sy * w + sx] - 128.f;
                            float rq = crf[sy * w + sx] - 128.f;
                            float rr = yq + 1.402f * rq;
                            float gg = yq - 0.344136f * cq -
                                       0.714136f * rq;
                            float bb = yq + 1.772f * cq;
                            r2 += rr < 0 ? 0 : (rr > 255 ? 255 : rr);
                            g2 += gg < 0 ? 0 : (gg > 255 ? 255 : gg);
                            b2 += bb < 0 ? 0 : (bb > 255 ? 255 : bb);
                            n++;
                        }
                    }
                }
                if (n) {
                    r2 /= n;
                    g2 /= n;
                    b2 /= n;
                }
                U[(y / 2) * (w / 2) + x / 2] = clamp_u8(
                    -0.168736f * r2 - 0.331264f * g2 + 0.5f * b2 + 128.f);
                V[(y / 2) * (w / 2) + x / 2] = clamp_u8(
                    0.5f * r2 - 0.418688f * g2 - 0.081312f * b2 + 128.f);
            }
        }
    }
}

// ------------------------------------------------------------ public API
void THPInit(void)
{
    cos_init();
}

THPFileInfo* THPVideoDecode(void* header, void* status_out,
                            THPFileInfo* work, void* data,
                            THPDec_8032FD40_Data* desc)
{
    float *yf = NULL, *cbf = NULL, *crf = NULL;
    int w = 0, h = 0;
    ThpCacheEnt* e;
    (void) header; // dimensions come from the JPEG SOF (authoritative)
    (void) desc;
    if (status_out)
        *(s32*) status_out = 0;
    if (!data || !work)
        return work;
    if (jpeg_decode_frame((const u8*) data,
                          (const u8*) data + (16u * 1024u * 1024u), &yf,
                          &cbf, &crf, &w, &h) &&
        w > 0 && h > 0) {
        e = thp_cache_get(work, w, h);
        if (e) {
            rgb_to_yuv420(yf, cbf, crf, w, h, e->y, e->u, e->v);
        }
    }
    // on failure the previous cached frame repeats (freeze, no hang)
    free(yf);
    free(cbf);
    free(crf);
    return work;
}

s32 THPDec_803302EC(u8** data)
{
    (void) data;
    return 0;
}

s32 THPDec_8032FD40(THPDec_8032FD40_Data* arg0, u16 arg1)
{
    // Work-area size for the decoder scratch. Ours lives in the side
    // cache, so only a token amount is needed for the area lbmthp bumps
    // past (unk_98).
    (void) arg0;
    (void) arg1;
    return 4096;
}

s32 THPDec_8032F8D4(u8* data, THPDec_8032FD40_Data* out)
{
    (void) data;
    (void) out;
    return 0;
}

u8 THPDec_80330158(THPFileInfo* info)
{
    (void) info;
    return 1; // frame ready
}

static void thp_copy_planes(THPFileInfo* info, void* y, void* u, void* v)
{
    ThpCacheEnt* e = thp_cache_find(info);
    if (!e) {
        return; // no frame decoded yet; planes stay as allocated (black)
    }
    {
        size_t ys = (size_t) e->w * e->h, uvs = ys / 4;
        if (y)
            memcpy(y, e->y, ys);
        if (u)
            memcpy(u, e->u, uvs);
        if (v)
            memcpy(v, e->v, uvs);
    }
}

void THPDec_80331340(THPFileInfo* info, void* y, void* u, void* v)
{
    thp_copy_planes(info, y, u, v);
}

void THPDec_803313D0(THPFileInfo* info, void* y, void* u, void* v, u32 w)
{
    (void) w; // plane geometry matches the decoded frame by construction
    thp_copy_planes(info, y, u, v);
}

void THPDec_803300E0(THPFileInfo* info)
{
    (void) info;
}
