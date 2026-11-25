/**
 * @file img_dither.c
 * @brief Bilinear cover-fit + serpentine Floyd–Steinberg onto the native frame.
 *
 * The URL path used to dither at the decoded resolution (400×300 for a
 * 1600×1200 JPEG, less for a small picture) and then blow the ink codes up by
 * nearest sampling: every dither dot became a 4×4 block on the panel, 6×6 for
 * a wide logo, and mid-tones turned into coarse speckle. Here the picture is
 * upsampled smoothly and dithered on a grid of up to one dot per panel pixel.
 *
 * Two things keep grey content grey. Spectra 6 has six inks and no grey, and
 * per-channel error diffusion used to pick colour inks to cancel a channel
 * error — grey text came out speckled red, yellow and blue. A near-neutral
 * pixel of the picture now only picks black or white, and its error is
 * booked against a neutral white. And the nearest-ink metric is the same
 * weighted distance as before, evaluated once into a table instead of six
 * float distances per pixel (50 µs a pixel on the C61).
 *
 * The scan runs along a logical column, which is a native line: the panel's
 * frame is column-major, so each column is dithered top to bottom (alternating
 * direction, as a serpentine) and its 300 bytes per IC half are written out
 * in one go. The first version scanned rows and did a read-modify-write per
 * pixel at a 300-byte stride — a PSRAM cache miss for every pixel of the
 * panel, 8.8 s a picture.
 *
 * The tables are built per call into the working memory and released with it:
 * a raw frame is fetched with about 90 KB of PSRAM to spare, so nothing may
 * be kept there between displays.
 *
 * Pure C (no ESP-IDF): the host test compiles this file as it is.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "img_dither.h"

#define NATIVE_W   1200              /* nx: source lines — CS0 0..599, CS1 600..1199 */
#define NATIVE_H   1600              /* ny: gate lines = logical x                   */
#define CS_PX      600
#define LINE_BYTES 300               /* 600 px, 4 bits each                          */
#define HALF_BYTES ((size_t)LINE_BYTES * NATIVE_H)
#define HALF_ROWS  (IMG_DITHER_PANEL_H / 2)   /* logical rows driven by one IC half */

/* Soft panel-like RGB of the six inks and the nibble the panel takes for it. */
static const uint8_t kInkRgb[6][3] = {
    {22, 22, 22}, {248, 248, 242}, {228, 198, 42}, {188, 42, 40}, {42, 72, 158}, {42, 122, 68},
};
static const uint8_t kInkCode[6] = {0x0, 0x1, 0x2, 0x3, 0x5, 0x6};
static const uint8_t kInkIndex[16] = {0, 1, 2, 3, 1, 4, 5, 1, 1, 1, 1, 1, 1, 1, 1, 1};
#define INK_WHITE 0x1

/* Channels differing by less than this make a grey: black or white only. */
#define NEUTRAL_SPREAD 8

/* 4 bits a channel: both tables are 8 KB together and stay in the cache. */
#define LUT_BITS 4
#define LUT_BYTES (1 << (3 * LUT_BITS))
#define LUT_SHIFT (8 - LUT_BITS)

/* Same curve the web console applies before it dither: x^0.92, contrast 1.12. */
static void prep_table(uint8_t *prep)
{
    for (int c = 0; c < 256; c++) {
        float x = powf(c / 255.f, 0.92f);
        x = (x - 0.5f) * 1.12f + 0.5f;
        prep[c] = (uint8_t)(fminf(fmaxf(x, 0.f), 1.f) * 255.f + 0.5f);
    }
}

/* Nearest ink by the weighted distance the firmware has always used:
 *   (2 + rm/256)·dr² + 4·dg² + (2 + (255 − rm)/256)·db²,  rm = (r + ink_r) / 2
 * written as integers (×512): exact, and no float work per pixel. */
static uint8_t nearest_ink(int r, int g, int b, int n_inks, const uint8_t ref[][3])
{
    int32_t best = INT32_MAX;
    uint8_t ink = INK_WHITE;
    for (int i = 0; i < n_inks; i++) {
        int rm2 = r + ref[i][0];
        int dr = r - ref[i][0];
        int dg = g - ref[i][1];
        int db = b - ref[i][2];
        int32_t d = (int32_t)(1024 + rm2) * dr * dr + 2048 * dg * dg +
                    (int32_t)(1534 - rm2) * db * db;
        if (d < best) {
            best = d;
            ink = kInkCode[i];
        }
    }
    return ink;
}

/* Fixed-point source coordinate of a dither line: first texel (−1 when it
 * lies outside the cover) and the weight of the next one. */
typedef struct {
    int16_t first;
    uint8_t next_weight;
} texel_t;

static texel_t locate(float f, int n)
{
    texel_t t = {-1, 0};
    if (f >= -0.5f && f <= (float)n - 0.5f) {
        int base = (int)floorf(f);
        if (base > n - 1) {
            base = n - 1;
        }
        t.next_weight = (uint8_t)((int)((f - base) * 256.f + 0.5f) & 255);
        t.first = (int16_t)(base < 0 ? 0 : base);
    }
    return t;
}

int img_dither_native(const uint8_t *rgb, int w, int h, int grid_w, int grid_h,
                      uint8_t *frame, void *(*alloc)(size_t n), void (*kick)(void))
{
    if (!rgb || !frame || !alloc || w < 1 || h < 1 || w > 32767 || h > 32767 ||
        grid_w < 1 || grid_h < 1 || grid_w > IMG_DITHER_PANEL_W ||
        IMG_DITHER_PANEL_W % grid_w != 0 || IMG_DITHER_PANEL_H % grid_h != 0) {
        return -1;
    }
    const int step_x = IMG_DITHER_PANEL_W / grid_w;    /* panel pixels a dot covers */
    const int step_y = IMG_DITHER_PANEL_H / grid_h;

    /* [ink table][neutral table][two error columns][source row of every dither
     * row][source column of every dither column][one output line per IC half] */
    const size_t err_off = 2 * LUT_BYTES;
    const size_t err_bytes = (size_t)grid_h * 3 * sizeof(int16_t);
    const size_t row_off = err_off + 2 * err_bytes;
    const size_t col_off = row_off + (size_t)grid_h * sizeof(texel_t);
    const size_t buf_off = col_off + (size_t)grid_w * sizeof(texel_t);
    uint8_t *work = alloc(buf_off + 2 * LINE_BYTES);
    if (!work) {
        return -1;
    }
    uint8_t *lut = work;                    /* every ink        */
    uint8_t *nlut = work + LUT_BYTES;       /* black or white   */
    int16_t *err_a = (int16_t *)(work + err_off);
    int16_t *err_b = err_a + (size_t)grid_h * 3;
    const texel_t *rows = (const texel_t *)(work + row_off);
    const texel_t *cols = (const texel_t *)(work + col_off);
    uint8_t *const line0 = work + buf_off;  /* logical rows 600..1199 → CS0 half */
    uint8_t *const line1 = line0 + LINE_BYTES; /* logical rows 0..599 → CS1 half */

    uint8_t prep[256];
    prep_table(prep);
    /* The inks go through the same curve as the picture. Comparing a prepped
     * pixel with the raw ink colours left every pixel of a pure-colour area a
     * few levels of "error" that the diffusion had to carry on: values
     * saturated within tens of pixels, and a saturated area pushed a constant
     * stream of error into the first column of its neighbour — 190 stray blue
     * dots in a green block next to a blue one. */
    uint8_t ink_prep[6][3], white_prep[3] = {248, 248, 248};
    for (int i = 0; i < 6; i++) {
        for (int c = 0; c < 3; c++) {
            ink_prep[i][c] = prep[kInkRgb[i][c]];
        }
    }
    for (int c = 0; c < 3; c++) {
        white_prep[c] = prep[white_prep[c]];
    }
    for (int ri = 0; ri < (1 << LUT_BITS); ri++) {
        for (int gi = 0; gi < (1 << LUT_BITS); gi++) {
            for (int bi = 0; bi < (1 << LUT_BITS); bi++) {
                size_t i = ((size_t)ri << (2 * LUT_BITS)) | ((size_t)gi << LUT_BITS) | bi;
                int r = (ri << LUT_SHIFT) + (1 << (LUT_SHIFT - 1));
                int g = (gi << LUT_SHIFT) + (1 << (LUT_SHIFT - 1));
                int b = (bi << LUT_SHIFT) + (1 << (LUT_SHIFT - 1));
                lut[i] = nearest_ink(r, g, b, 6, ink_prep);
                nlut[i] = nearest_ink(r, g, b, 2, ink_prep);
            }
        }
    }

    /* Cover fit of the source onto the dither grid (in grid units). */
    float s = (float)grid_w / (float)w;
    const float sy = (float)grid_h / (float)h;
    if (sy > s) {
        s = sy;
    }
    const float dw = (float)w * s, dh = (float)h * s;
    const float ox = ((float)grid_w - dw) * 0.5f, oy = ((float)grid_h - dh) * 0.5f;
    const float x_scale = (float)w / dw, y_scale = (float)h / dh;
    texel_t *rows_w = (texel_t *)(work + row_off), *cols_w = (texel_t *)(work + col_off);
    for (int gy = 0; gy < grid_h; gy++) {
        rows_w[gy] = locate(((float)gy + 0.5f - oy) * y_scale - 0.5f, h);
    }
    for (int gx = 0; gx < grid_w; gx++) {
        cols_w[gx] = locate(((float)gx + 0.5f - ox) * x_scale - 0.5f, w);
    }
    memset(err_a, 0, 2 * err_bytes);        /* the first column starts clean */

    const int soft_cap = 210;
    /* A byte of the frame holds two neighbouring logical rows; on a 800×600
     * grid it holds the four rows of one dot. Both index runs go downwards. */
    const int up_base = HALF_ROWS - 1, lo_base = HALF_ROWS / 2 - 1;

    for (int gx = 0; gx < grid_w; gx++) {
        const int ltr = (gx & 1) == 0;              /* even columns scan downwards */
        const int dir = ltr ? -1 : 1;
        int16_t *cur = ltr ? err_a : err_b;         /* this column: the pixel ahead   */
        int16_t *nxt = ltr ? err_b : err_a;         /* next column                    */
        memset(nxt, 0, err_bytes);
        const texel_t c = cols[gx];
        const int s0 = c.first, tx = c.next_weight, wx = 256 - tx;
        const int s1 = s0 + 1 < w ? s0 + 1 : s0;
        const int in_cover = s0 >= 0;
        int pend = 0;                               /* first nibble of a pair */
        int gy = ltr ? grid_h - 1 : 0;

        for (int i = 0; i < grid_h; i++, gy += dir) {
            uint8_t ink = INK_WHITE;                /* outside the cover: white, no error */
            uint8_t *line = gy >= HALF_ROWS / step_y ? line0 : line1;
            int index = (gy >= HALF_ROWS / step_y ? up_base : lo_base) -
                        (step_y == 2 ? gy : (gy >> 1));
            if (in_cover && rows[gy].first >= 0) {
                const int r0 = rows[gy].first, ty = rows[gy].next_weight, wy = 256 - ty;
                const int r1 = r0 + 1 < h ? r0 + 1 : r0;
                const uint8_t *p00 = rgb + ((size_t)r0 * w + s0) * 3;
                const uint8_t *p01 = rgb + ((size_t)r0 * w + s1) * 3;
                const uint8_t *p10 = rgb + ((size_t)r1 * w + s0) * 3;
                const uint8_t *p11 = rgb + ((size_t)r1 * w + s1) * 3;
                const int16_t *e = cur + (size_t)gy * 3;
#define BILINEAR(c) (((p00[c] * wx + p01[c] * tx) * wy + (p10[c] * wx + p11[c] * tx) * ty) >> 16)
                const int sr = prep[BILINEAR(0)], sg = prep[BILINEAR(1)], sb = prep[BILINEAR(2)];
#undef BILINEAR
                int r = sr + e[0];
                int g = sg + e[1];
                int b = sb + e[2];
                if (r < 0) {
                    r = 0;
                } else if (r > 255) {
                    r = 255;
                }
                if (g < 0) {
                    g = 0;
                } else if (g > 255) {
                    g = 255;
                }
                if (b < 0) {
                    b = 0;
                } else if (b > 255) {
                    b = 255;
                }
                const size_t idx = ((size_t)(r >> LUT_SHIFT) << (2 * LUT_BITS)) |
                                   ((size_t)(g >> LUT_SHIFT) << LUT_BITS) | (size_t)(b >> LUT_SHIFT);
                /* Grey is decided on the picture, not on the error-corrected
                 * value. The white ink's own tint (248,248,242) parts the
                 * channels by 8 a white dot; judged on the corrected value a
                 * grey area drifted into the colour table within three rows
                 * and came out 79 % colour. Its error is booked against a
                 * neutral white, so the channels stay together. */
                int s_hi = sr > sg ? sr : sg, s_lo = sr < sg ? sr : sg;
                if (sb > s_hi) {
                    s_hi = sb;
                }
                if (sb < s_lo) {
                    s_lo = sb;
                }
                const uint8_t *prgb;
                if (s_hi - s_lo < NEUTRAL_SPREAD) {
                    ink = nlut[idx];
                    prgb = (ink == INK_WHITE) ? white_prep : ink_prep[0];
                } else {
                    ink = lut[idx];
                    prgb = ink_prep[kInkIndex[ink]];
                }
                int er = r - prgb[0], eg = g - prgb[1], eb = b - prgb[2];
                if (er > soft_cap) {
                    er = soft_cap;
                } else if (er < -soft_cap) {
                    er = -soft_cap;
                }
                if (eg > soft_cap) {
                    eg = soft_cap;
                } else if (eg < -soft_cap) {
                    eg = -soft_cap;
                }
                if (eb > soft_cap) {
                    eb = soft_cap;
                } else if (eb < -soft_cap) {
                    eb = -soft_cap;
                }
                const int ahead = gy + dir;    /* next pixel of this column */
#define ADD_ERR(col, yy, num)                                                                       \
    do {                                                                                            \
        if ((yy) >= 0 && (yy) < grid_h) {                                                           \
            int16_t *q = (col) + (size_t)(yy) * 3;                                                  \
            q[0] = (int16_t)(q[0] + er * (num) / 16);                                               \
            q[1] = (int16_t)(q[1] + eg * (num) / 16);                                               \
            q[2] = (int16_t)(q[2] + eb * (num) / 16);                                               \
        }                                                                                           \
    } while (0)
                ADD_ERR(cur, ahead, 7);
                if (gx + 1 < grid_w) {      /* the next column: behind, same, ahead */
                    ADD_ERR(nxt, gy - dir, 3);
                    ADD_ERR(nxt, gy, 5);
                    ADD_ERR(nxt, ahead, 1);
                }
#undef ADD_ERR
            }
            if (step_y == 2) {
                line[index] = (uint8_t)(ink * 0x11);
            } else if ((gy & 1) == (ltr ? 1 : 0)) {
                pend = ink;                 /* odd logical row: high nibble */
            } else if (ltr) {
                line[index] = (uint8_t)((pend << 4) | ink);   /* went odd → even */
            } else {
                line[index] = (uint8_t)((ink << 4) | pend);   /* went even → odd */
            }
        }
        for (int dx = 0; dx < step_x; dx++) {
            const size_t off = (size_t)(gx * step_x + dx) * LINE_BYTES;
            memcpy(frame + off, line0, LINE_BYTES);
            memcpy(frame + HALF_BYTES + off, line1, LINE_BYTES);
        }
        if (kick && (gx & 15) == 15) {
            kick();
        }
    }
    free(work);
    return 0;
}
