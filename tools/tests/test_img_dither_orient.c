/* main/img_dither.c on the host, with the EXIF orientation: a picture that a
 * phone stored on its side or upside down, dithered with its tag, must give
 * the frame of the same picture dithered as it stands — byte for byte, for
 * all eight orientations. Run by tools/run_host_tests.py. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "img_dither.h"

#define FRAME_BYTES 960000u

enum { UW = 32, UH = 16 };        /* the upright picture */

static int fails, checks;

static void expect(int cond, const char *what)
{
    checks++;
    if (!cond) {
        fails++;
        printf("FAIL %s\n", what);
    }
}

/* Asymmetric in both directions, and smooth enough to dither. */
static void upright_pixel(int x, int y, uint8_t *rgb)
{
    rgb[0] = (uint8_t)(40 + x * 6);
    rgb[1] = (uint8_t)(230 - y * 10);
    rgb[2] = (uint8_t)(20 + x * y);
}

/* The picture as a camera stores it for orientation `o`: the inverse of what
 * the dither does. */
static void stored(const uint8_t *up, int o, uint8_t *out, int *sw, int *sh)
{
    const int swap = o >= 5, fx = (o - 1) & 1, fy = (o - 1) & 2;
    *sw = swap ? UH : UW;
    *sh = swap ? UW : UH;
    for (int ys = 0; ys < *sh; ys++) {
        for (int xs = 0; xs < *sw; xs++) {
            int xu, yu;
            if (!swap) {
                xu = fx ? UW - 1 - xs : xs;
                yu = fy ? UH - 1 - ys : ys;
            } else {
                xu = fx ? UW - 1 - ys : ys;       /* the stored row is the upright column */
                yu = fy ? UH - 1 - xs : xs;
            }
            memcpy(out + ((size_t)ys * *sw + xs) * 3, up + ((size_t)yu * UW + xu) * 3, 3);
        }
    }
}

static uint8_t up[UW * UH * 3], st[UW * UH * 3];
static uint8_t frame[FRAME_BYTES], ref[FRAME_BYTES];

static void *plain_alloc(size_t n)
{
    return malloc(n);
}

static void dither(const uint8_t *rgb, int w, int h, int o, int gw, int gh, uint8_t *out)
{
    memset(out, 0xA5, FRAME_BYTES);
    if (img_dither_native_o(rgb, w, h, o, gw, gh, out, plain_alloc, NULL) != 0) {
        printf("FAIL dither returned -1 (w=%d h=%d o=%d grid=%dx%d)\n", w, h, o, gw, gh);
        fails++;
    }
}

int main(void)
{
    for (int y = 0; y < UH; y++) {
        for (int x = 0; x < UW; x++) {
            upright_pixel(x, y, up + ((size_t)y * UW + x) * 3);
        }
    }

    /* on both grids */
    const int grids[2][2] = {{800, 600}, {1600, 1200}};
    for (int g = 0; g < 2; g++) {
        const int gw = grids[g][0], gh = grids[g][1];
        dither(up, UW, UH, 1, gw, gh, ref);

        /* the plain entry point is orientation 1 */
        dither(up, UW, UH, 0, gw, gh, frame);      /* out of range: treated as 1 */
        expect(memcmp(frame, ref, FRAME_BYTES) == 0, "an unknown orientation is taken as 1");
        dither(up, UW, UH, 9, gw, gh, frame);
        expect(memcmp(frame, ref, FRAME_BYTES) == 0, "orientation 9 is taken as 1");

        for (int o = 2; o <= 8; o++) {
            int sw, sh;
            stored(up, o, st, &sw, &sh);
            dither(st, sw, sh, o, gw, gh, frame);
            checks++;
            if (memcmp(frame, ref, FRAME_BYTES) != 0) {
                fails++;
                printf("FAIL orientation %d on the %dx%d grid differs from the upright picture\n",
                       o, gw, gh);
            }
            /* the same stored picture without its tag must not pass for the
             * upright one — otherwise the check above proves nothing */
            dither(st, sw, sh, 1, gw, gh, frame);
            checks++;
            if (memcmp(frame, ref, FRAME_BYTES) == 0) {
                fails++;
                printf("FAIL orientation %d on the %dx%d grid: the stored picture looks the same "
                       "without its tag\n", o, gw, gh);
            }
        }
        /* and the upright picture with a tag on it is not the upright one */
        dither(up, UW, UH, 6, gw, gh, frame);
        expect(memcmp(frame, ref, FRAME_BYTES) != 0,
               "the upright picture turned by 6 is not the upright picture");

        /* the entry point without an orientation: unchanged frame */
        memset(frame, 0xA5, FRAME_BYTES);
        if (img_dither_native(up, UW, UH, gw, gh, frame, plain_alloc, NULL) != 0) {
            fails++;
            printf("FAIL img_dither_native returned -1\n");
        }
        expect(memcmp(frame, ref, FRAME_BYTES) == 0,
               "img_dither_native is img_dither_native_o with orientation 1");
    }

    if (fails == 0) {
        printf("RESULT: pass - %d checks: 8 orientations give the upright frame on both grids, "
               "14 negative controls differ\n", checks);
        return 0;
    }
    printf("RESULT: fail - %d of %d checks\n", fails, checks);
    return 1;
}
