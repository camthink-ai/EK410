/* Runs main/img_dither.c from the command line for the host tests:
       img_dither_cli W H GRID_W GRID_H < rgb  > frame
   (rgb: W*H*3 bytes; frame: the 960000-byte native layout) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "img_dither.h"

#define FRAME_BYTES 960000u

int main(int argc, char **argv)
{
    if (argc != 5) {
        return 2;
    }
    const int w = atoi(argv[1]), h = atoi(argv[2]);
    const int gw = atoi(argv[3]), gh = atoi(argv[4]);
    const size_t n = (size_t)(w > 0 && h > 0 ? w * h : 0) * 3;
    uint8_t *rgb = malloc(n ? n : 1);
    uint8_t *frame = malloc(FRAME_BYTES);
    if (!rgb || !frame) {
        return 3;
    }
    if (n == 0 || fread(rgb, 1, n, stdin) != n) {
        return 4;
    }
    if (img_dither_native(rgb, w, h, gw, gh, frame, malloc, NULL) != 0) {
        return 5;
    }
    if (fwrite(frame, 1, FRAME_BYTES, stdout) != FRAME_BYTES) {
        return 6;
    }
    return 0;
}
