/* main/img_dither.c on the host: the native frame it writes must be
 * complete, keep grey content grey, keep the six inks and preserve tone.
 * Run by tools/run_host_tests.py; test_img_dither.py adds the layout
 * cross-check against tools/frame_convert.py. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "img_dither.h"

#define FRAME_BYTES 960000u

enum { BLACK = 0x0, WHITE = 0x1, YELLOW = 0x2, RED = 0x3, BLUE = 0x5, GREEN = 0x6 };

static const uint8_t kRgb[16][3] = {
    {22, 22, 22}, {248, 248, 242}, {228, 198, 42}, {188, 42, 40}, {0, 0, 0},
    {42, 72, 158}, {42, 122, 68},
};

static int fails, checks, kicks;

static void expect(int cond, const char *what)
{
    checks++;
    if (!cond) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

static void kick(void)
{
    kicks++;
}

static void *failing_alloc(size_t n)
{
    (void)n;
    return NULL;
}

static int is_ink(uint8_t c)
{
    return c == BLACK || c == WHITE || c == YELLOW || c == RED || c == BLUE || c == GREEN;
}

/* Independent of the module: how last_frame.c reads a logical pixel back. */
static uint8_t px(const uint8_t *frame, int lx, int ly)
{
    int nx = 1200 - 1 - ly;
    int csx = nx / 600, local = nx % 600;
    uint8_t byte = frame[(size_t)csx * 480000u + (size_t)lx * 300u + (local >> 1)];
    return (local & 1) ? (byte & 0x0F) : (byte >> 4);
}

static int bad_nibbles(const uint8_t *frame)
{
    int n = 0;
    for (size_t i = 0; i < FRAME_BYTES; i++) {
        n += !is_ink(frame[i] >> 4) + !is_ink(frame[i] & 0x0F);
    }
    return n;
}

static long count_ink(const uint8_t *frame, uint8_t ink)
{
    long n = 0;
    for (int ly = 0; ly < 1200; ly++) {
        for (int lx = 0; lx < 1600; lx++) {
            n += px(frame, lx, ly) == ink;
        }
    }
    return n;
}

static void fill(uint8_t *rgb, int w, int x0, int y0, int x1, int y1, uint8_t code)
{
    for (int y = y0; y < y1; y++) {
        for (int x = x0; x < x1; x++) {
            memcpy(rgb + ((size_t)y * w + x) * 3, kRgb[code], 3);
        }
    }
}

static uint8_t *frame_new(void)
{
    uint8_t *f = malloc(FRAME_BYTES);
    if (!f) {
        exit(3);
    }
    memset(f, 0xEE, FRAME_BYTES); /* 0xE is no ink: every nibble must be written */
    return f;
}

static void test_six_inks_full_grid(void)
{
    const int w = 1600, h = 1200;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    uint8_t *frame = frame_new();
    if (!rgb) {
        exit(3);
    }
    /* 3 x 2 blocks, boundaries on even pixels (so the 800x600 grid agrees) */
    const int bx[4] = {0, 534, 1066, 1600}, by[3] = {0, 600, 1200};
    const uint8_t order[6] = {BLACK, WHITE, YELLOW, RED, BLUE, GREEN};
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 3; c++) {
            fill(rgb, w, bx[c], by[r], bx[c + 1], by[r + 1], order[r * 3 + c]);
        }
    }
    kicks = 0;
    expect(img_dither_native(rgb, w, h, 1600, 1200, frame, malloc, kick) == 0, "full grid: rc");
    expect(bad_nibbles(frame) == 0, "full grid: every panel pixel written");
    expect(kicks == 1600 / 16, "full grid: kick every 16 columns");
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 3; c++) {
            const int lx = (bx[c] + bx[c + 1]) / 2, ly = (by[r] + by[r + 1]) / 2;
            char what[64];
            snprintf(what, sizeof(what), "full grid: ink 0x%x block", order[r * 3 + c]);
            expect(px(frame, lx, ly) == order[r * 3 + c], what);
        }
    }
    /* a single-pixel column inside the blue block: on the full grid a 1 px
     * edge stays 1 px */
    fill(rgb, w, 800, 600, 801, 1200, WHITE);
    expect(img_dither_native(rgb, w, h, 1600, 1200, frame, malloc, NULL) == 0, "1 px column: rc");
    expect(px(frame, 800, 900) == WHITE && px(frame, 799, 900) == BLUE &&
               px(frame, 801, 900) == BLUE, "1 px column kept on the full grid");

    /* 800x600 grid: the same blocks, every dot 2x2 panel pixels */
    fill(rgb, w, 800, 600, 801, 1200, BLUE);
    expect(img_dither_native(rgb, w, h, 800, 600, frame, malloc, NULL) == 0, "half grid: rc");
    expect(bad_nibbles(frame) == 0, "half grid: every panel pixel written");
    expect(px(frame, 267, 300) == BLACK && px(frame, 800, 300) == WHITE &&
               px(frame, 1333, 300) == YELLOW && px(frame, 100, 900) == RED &&
               px(frame, 800, 900) == BLUE && px(frame, 1500, 900) == GREEN,
           "half grid: inks");
    /* a 1 px source column now straddles one grid column: whatever ink it
     * dithers to, the two panel pixels of that dot must agree */
    int paired = 1;
    for (int ly = 600; ly < 1200; ly++) {
        paired = paired && px(frame, 800, ly) == px(frame, 801, ly) &&
                 px(frame, 798, ly) == px(frame, 799, ly) &&
                 px(frame, 802, ly) == px(frame, 803, ly);
    }
    expect(paired, "half grid: dots are 2 px wide");
    /* Nibble order: a byte of the frame holds two neighbouring logical rows,
     * odd one in the high nibble. Uniform blocks cannot tell a swap — a
     * swapped column scan passed every check above. */
    for (int odd_black = 0; odd_black < 2; odd_black++) {
        for (int y = 0; y < h; y++) {
            fill(rgb, w, 0, y, w, y + 1, ((y & 1) == odd_black) ? BLACK : WHITE);
        }
        expect(img_dither_native(rgb, w, h, 1600, 1200, frame, malloc, NULL) == 0,
               "row stripes: rc");
        long bad = 0;
        for (size_t i = 0; i < FRAME_BYTES; i++) {
            bad += frame[i] != (odd_black ? 0x01 : 0x10);
        }
        expect(bad == 0, "row stripes: odd row in the high nibble, even in the low");
    }
    free(rgb);
    free(frame);
}

static void test_grey_stays_grey(void)
{
    const int w = 1600, h = 1200;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    uint8_t *frame = frame_new();
    if (!rgb) {
        exit(3);
    }
    fill(rgb, w, 0, 0, w, h, WHITE);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = 70; /* dark grey text */
    }
    for (int half = 0; half < 2; half++) {
        const int gw = half ? 800 : 1600, gh = half ? 600 : 1200;
        expect(img_dither_native(rgb, w, h, gw, gh, frame, malloc, NULL) == 0, "grey: rc");
        const long total = 1600L * 1200;
        const long black = count_ink(frame, BLACK), white = count_ink(frame, WHITE);
        expect(black + white == total, "grey: only black and white, no colour ink");
        /* (248 - prep(70)) / (248 - 22) ≈ 77 % black: the tone survives */
        expect(black * 100 / total > 72 && black * 100 / total < 82, "grey: tone preserved");
    }
    /* a slight tint below the neutral threshold is still grey */
    for (size_t i = 0; i < (size_t)w * h; i++) {
        rgb[i * 3] = 72;
        rgb[i * 3 + 1] = 70;
        rgb[i * 3 + 2] = 66;
    }
    expect(img_dither_native(rgb, w, h, 1600, 1200, frame, malloc, NULL) == 0, "tinted grey: rc");
    expect(count_ink(frame, BLACK) + count_ink(frame, WHITE) == 1600L * 1200,
           "tinted grey: still no colour ink");
    /* a clear colour keeps its colour inks */
    for (size_t i = 0; i < (size_t)w * h; i++) {
        rgb[i * 3] = 200;
        rgb[i * 3 + 1] = 120;
        rgb[i * 3 + 2] = 40; /* orange: between yellow and red */
    }
    expect(img_dither_native(rgb, w, h, 1600, 1200, frame, malloc, NULL) == 0, "orange: rc");
    expect(count_ink(frame, YELLOW) > 100000 && count_ink(frame, RED) > 100000,
           "orange: mixed from yellow and red");
    free(rgb);
    free(frame);
}

static void test_cover_fit(void)
{
    /* a small square picture fills the whole panel (cover), 8x upscaled */
    const int w = 200, h = 200;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    uint8_t *frame = frame_new();
    if (!rgb) {
        exit(3);
    }
    fill(rgb, w, 0, 0, w, h, RED);
    expect(img_dither_native(rgb, w, h, 1600, 1200, frame, malloc, NULL) == 0, "cover: rc");
    expect(bad_nibbles(frame) == 0, "cover: every panel pixel written");
    expect(count_ink(frame, RED) == 1600L * 1200, "cover: small square fills the panel");
    free(rgb);
    free(frame);
}

static void test_tone_of_a_gradient(void)
{
    /* grey ramp from white (left) to black (right) on a small picture */
    const int w = 64, h = 48;
    uint8_t *rgb = malloc((size_t)w * h * 3);
    uint8_t *frame = frame_new();
    if (!rgb) {
        exit(3);
    }
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t v = (uint8_t)(255 - x * 4);
            memset(rgb + ((size_t)y * w + x) * 3, v, 3);
        }
    }
    expect(img_dither_native(rgb, w, h, 800, 600, frame, malloc, NULL) == 0, "ramp: rc");
    int ok = 1;
    int prev_black = -1;
    for (int band = 0; band < 8; band++) {
        long black = 0;
        for (int ly = 0; ly < 1200; ly++) {
            for (int lx = band * 200; lx < band * 200 + 200; lx++) {
                black += px(frame, lx, ly) == BLACK;
            }
        }
        if (prev_black >= 0 && black <= prev_black) {
            ok = 0;
        }
        prev_black = (int)black;
    }
    expect(ok, "ramp: each band darker than the one before");
    free(rgb);
    free(frame);
}

static void test_failure_leaves_the_frame_alone(void)
{
    uint8_t *frame = frame_new(), *copy = malloc(FRAME_BYTES);
    uint8_t rgb[3] = {188, 42, 40};
    if (!copy) {
        exit(3);
    }
    memcpy(copy, frame, FRAME_BYTES);
    expect(img_dither_native(rgb, 1, 1, 1600, 1200, frame, failing_alloc, NULL) == -1,
           "no memory: rc -1");
    expect(memcmp(copy, frame, FRAME_BYTES) == 0, "no memory: frame untouched");
    expect(img_dither_native(rgb, 1, 1, 1000, 1200, frame, malloc, NULL) == -1,
           "grid 1000 wide: refused");
    expect(img_dither_native(rgb, 1, 1, 1600, 1200, NULL, malloc, NULL) == -1,
           "no frame: refused");
    expect(img_dither_native(NULL, 1, 1, 1600, 1200, frame, malloc, NULL) == -1,
           "no source: refused");
    free(frame);
    free(copy);
}

int main(void)
{
    test_six_inks_full_grid();
    test_grey_stays_grey();
    test_cover_fit();
    test_tone_of_a_gradient();
    test_failure_leaves_the_frame_alone();
    printf("RESULT: %s - %d checks, %d failed\n", fails ? "fail" : "pass", checks, fails);
    return fails ? 1 : 0;
}
