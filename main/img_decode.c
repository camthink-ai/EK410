/**
 * @file img_decode.c
 * @brief JPEG (ROM TJpgDec) + PNG (libpng) → Spectra 4bpp logical framebuffer.
 *
 * SoftAP uses full-panel FS in the browser (~seconds there). On C61 that path
 * took ~100s and starved the idle TWDT — so URL decode dithers at a budgeted
 * resolution (softCap 210, same E6/prep) then cover-samples into the panel.
 * SPEED ≈360KB RGB; QUALITY ≈720KB (+ optional nearest upscale before FS).
 * Best quality still: SoftAP Image upload or platform-side raw 4bpp 960000.
 */
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rom/tjpgd.h"
#include "png.h"

#include "img_decode.h"

static const char *TAG = "img_dec";

/* Soft panel-like RGB — same anchors as SoftAP E6_INK. */
static const struct {
    uint8_t r, g, b;
    uint8_t ink;
} kE6[] = {
    {22, 22, 22, 0x0},     /* black  */
    {248, 248, 242, 0x1},  /* white  */
    {228, 198, 42, 0x2},   /* yellow */
    {188, 42, 40, 0x3},    /* red    */
    {42, 72, 158, 0x5},    /* blue   */
    {42, 122, 68, 0x6},    /* green  */
};

/* Source RGB budgets beside Wi-Fi/MQTT on 2MB PSRAM (body still held at decode).
 * Clamped to the largest free SPIRAM block so a big PNG body does not OOM. */
#define RGB_BUDGET_SPEED   (360 * 1024)
#define RGB_BUDGET_QUALITY (720 * 1024)
#define RGB_BUDGET_FLOOR   (48 * 1024)
#define RGB_BUDGET_RESERVE (48 * 1024) /* one PNG row + libpng + slack */

static size_t rgb_budget_for(img_proc_mode_t mode)
{
    size_t want = (mode == IMG_PROC_QUALITY) ? RGB_BUDGET_QUALITY : RGB_BUDGET_SPEED;
    size_t free_s = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    size_t room = (largest > RGB_BUDGET_RESERVE) ? (largest - RGB_BUDGET_RESERVE) : 0;
    if (room == 0 && free_s > RGB_BUDGET_RESERVE) {
        room = free_s - RGB_BUDGET_RESERVE;
    }
    if (room < want) {
        want = room;
    }
    if (want < RGB_BUDGET_FLOOR) {
        want = RGB_BUDGET_FLOOR;
    }
    ESP_LOGI(TAG, "rgb budget %uKB (mode=%s free_s=%u largest=%u)",
             (unsigned)(want / 1024),
             mode == IMG_PROC_QUALITY ? "quality" : "speed",
             (unsigned)free_s, (unsigned)largest);
    return want;
}

static uint8_t s_prep_lut[256];
static int s_prep_ready;

static void prep_lut_init(void)
{
    if (s_prep_ready) {
        return;
    }
    for (int c = 0; c < 256; c++) {
        float x = c / 255.f;
        x = powf(x, 0.92f);
        x = (x - 0.5f) * 1.12f + 0.5f;
        if (x < 0.f) {
            x = 0.f;
        }
        if (x > 1.f) {
            x = 1.f;
        }
        s_prep_lut[c] = (uint8_t)(x * 255.f + 0.5f);
    }
    s_prep_ready = 1;
}

static inline int clamp8i(int v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return v;
}

static inline void prep_eink_u8(int *r, int *g, int *b)
{
    *r = s_prep_lut[clamp8i(*r)];
    *g = s_prep_lut[clamp8i(*g)];
    *b = s_prep_lut[clamp8i(*b)];
}

static uint8_t nearest_ink_prepped(int r, int g, int b)
{
    float best = 1e30f;
    uint8_t ink = 0x1;
    for (int i = 0; i < 6; i++) {
        float rm = (r + kE6[i].r) / 2.f;
        float dr = (float)(r - kE6[i].r);
        float dg = (float)(g - kE6[i].g);
        float db = (float)(b - kE6[i].b);
        float d = (2.f + rm / 256.f) * dr * dr + 4.f * dg * dg +
                  (2.f + (255.f - rm) / 256.f) * db * db;
        if (d < best) {
            best = d;
            ink = kE6[i].ink;
        }
    }
    return ink;
}

static void ink_rgb(uint8_t ink, int *r, int *g, int *b)
{
    for (int i = 0; i < 6; i++) {
        if (kE6[i].ink == ink) {
            *r = kE6[i].r;
            *g = kE6[i].g;
            *b = kE6[i].b;
            return;
        }
    }
    *r = 248;
    *g = 248;
    *b = 242;
}

static void fb_fill(uint8_t *fb, uint8_t ink)
{
    uint8_t byte = (uint8_t)((ink << 4) | ink);
    memset(fb, byte, IMG_FB_BYTES);
}

static void fb_put(uint8_t *fb, int x, int y, uint8_t ink)
{
    if (x < 0 || y < 0 || x >= IMG_PANEL_W || y >= IMG_PANEL_H) {
        return;
    }
    size_t off = (size_t)y * (IMG_PANEL_W / 2) + (size_t)(x / 2);
    uint8_t b = fb[off];
    if (x & 1) {
        b = (uint8_t)((b & 0xF0) | (ink & 0x0F));
    } else {
        b = (uint8_t)((b & 0x0F) | ((ink & 0x0F) << 4));
    }
    fb[off] = b;
}

img_fmt_t img_sniff_format(const uint8_t *data, size_t len)
{
    if (!data || len < 3) {
        return IMG_FMT_UNKNOWN;
    }
    if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return IMG_FMT_JPEG;
    }
    if (len >= 8 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' && data[3] == 'G') {
        return IMG_FMT_PNG;
    }
    if (len >= 12 && data[0] == 'R' && data[1] == 'I' && data[2] == 'F' && data[3] == 'F' &&
        data[8] == 'W' && data[9] == 'E' && data[10] == 'B' && data[11] == 'P') {
        return IMG_FMT_WEBP;
    }
    if (len == IMG_FB_BYTES) {
        return IMG_FMT_RAW4BPP;
    }
    return IMG_FMT_UNKNOWN;
}

static void *spiram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(n);
    }
    return p;
}

static void kick_wdt(void)
{
    /* Must sleep — bare yield never runs IDLE, so TWDT fires on long dither. */
    vTaskDelay(1);
}

/* Singleton panel FB — once obtained, keep it so later URLs skip the 960KB alloc. */
static uint8_t *s_panel_fb;

void img_fb_release(uint8_t *fb)
{
    if (!fb || fb == s_panel_fb) {
        return;
    }
    free(fb);
}

static int panel_fb_ensure(void)
{
    if (s_panel_fb) {
        return 0;
    }
    s_panel_fb = heap_caps_malloc(IMG_FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_panel_fb) {
        s_panel_fb = malloc(IMG_FB_BYTES);
    }
    if (!s_panel_fb) {
        ESP_LOGE(TAG, "panel FB OOM (need %u, free %u largest %u)",
                 (unsigned)IMG_FB_BYTES,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return -1;
    }
    return 0;
}

/**
 * Serpentine FS at source res → codes packed into rgb[0..w*h).
 * softCap 210 matches SoftAP; pack+realloc frees ~2/3 before panel FB alloc.
 */
static int dither_rgb_inplace_codes(uint8_t *rgb, int w, int h)
{
    size_t err_bytes = (size_t)w * 3 * sizeof(int16_t);
    int16_t *err_a = spiram_alloc(err_bytes);
    int16_t *err_b = spiram_alloc(err_bytes);
    if (!err_a || !err_b) {
        free(err_a);
        free(err_b);
        return -1;
    }
    memset(err_a, 0, err_bytes);
    memset(err_b, 0, err_bytes);

    const int soft_cap = 210;
    for (int y = 0; y < h; y++) {
        int16_t *cur = (y % 2 == 0) ? err_a : err_b;
        int16_t *nxt = (y % 2 == 0) ? err_b : err_a;
        memset(nxt, 0, err_bytes);
        int ltr = (y % 2) == 0;
        for (int xi = 0; xi < w; xi++) {
            int x = ltr ? xi : (w - 1 - xi);
            size_t p = ((size_t)y * (size_t)w + (size_t)x) * 3;
            int r = clamp8i((int)rgb[p] + cur[x * 3]);
            int g = clamp8i((int)rgb[p + 1] + cur[x * 3 + 1]);
            int b = clamp8i((int)rgb[p + 2] + cur[x * 3 + 2]);
            prep_eink_u8(&r, &g, &b);
            uint8_t ink = nearest_ink_prepped(r, g, b);
            rgb[p + 1] = ink;
            int br, bg, bb;
            ink_rgb(ink, &br, &bg, &bb);
            int er = r - br;
            int eg = g - bg;
            int eb = b - bb;
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
#define ADD_ERR(row, xx, w16)                                                                       \
    do {                                                                                           \
        int _xx = (xx);                                                                            \
        if (_xx >= 0 && _xx < w) {                                                                 \
            (row)[_xx * 3] = (int16_t)((row)[_xx * 3] + er * (w16) / 16);                          \
            (row)[_xx * 3 + 1] = (int16_t)((row)[_xx * 3 + 1] + eg * (w16) / 16);                  \
            (row)[_xx * 3 + 2] = (int16_t)((row)[_xx * 3 + 2] + eb * (w16) / 16);                  \
        }                                                                                          \
    } while (0)
            if (ltr) {
                ADD_ERR(cur, x + 1, 7);
                ADD_ERR(nxt, x - 1, 3);
                ADD_ERR(nxt, x, 5);
                ADD_ERR(nxt, x + 1, 1);
            } else {
                ADD_ERR(cur, x - 1, 7);
                ADD_ERR(nxt, x + 1, 3);
                ADD_ERR(nxt, x, 5);
                ADD_ERR(nxt, x - 1, 1);
            }
#undef ADD_ERR
        }
        for (int x = 0; x < w; x++) {
            rgb[(size_t)y * (size_t)w + (size_t)x] =
                rgb[((size_t)y * (size_t)w + (size_t)x) * 3 + 1];
        }
        if ((y & 15) == 0) {
            kick_wdt();
        }
    }
    free(err_a);
    free(err_b);
    return 0;
}

static void cover_params(int sw, int sh, float *ox, float *oy, float *x_scale, float *y_scale)
{
    if (sw < 1) {
        sw = 1;
    }
    if (sh < 1) {
        sh = 1;
    }
    float s = (float)IMG_PANEL_W / (float)sw;
    float sy = (float)IMG_PANEL_H / (float)sh;
    if (sy > s) {
        s = sy;
    }
    float dw = (float)sw * s;
    float dh = (float)sh * s;
    *ox = ((float)IMG_PANEL_W - dw) * 0.5f;
    *oy = ((float)IMG_PANEL_H - dh) * 0.5f;
    *x_scale = (float)sw / dw;
    *y_scale = (float)sh / dh;
}

uint8_t img_codes_cover_pixel(const uint8_t *codes, int sw, int sh, int lx, int ly)
{
    if (!codes || sw < 1 || sh < 1) {
        return 0x1;
    }
    float ox, oy, x_scale, y_scale;
    cover_params(sw, sh, &ox, &oy, &x_scale, &y_scale);
    int syi = (int)(((float)ly + 0.5f - oy) * y_scale);
    int sxi = (int)(((float)lx + 0.5f - ox) * x_scale);
    if (syi < 0 || syi >= sh || sxi < 0 || sxi >= sw) {
        return 0x1;
    }
    return codes[(size_t)syi * (size_t)sw + (size_t)sxi];
}

/** Cover-fit ink codes onto logical 1600×1200 FB (SoftAP imgFit='cover'). */
static void codes_cover_to_fb(const uint8_t *codes, int sw, int sh, uint8_t *fb)
{
    float ox, oy, x_scale, y_scale;
    cover_params(sw, sh, &ox, &oy, &x_scale, &y_scale);
    if (sw < 1) {
        sw = 1;
    }
    if (sh < 1) {
        sh = 1;
    }

    fb_fill(fb, 0x1);
    for (int ly = 0; ly < IMG_PANEL_H; ly++) {
        int syi = (int)(((float)ly + 0.5f - oy) * y_scale);
        uint8_t *row = fb + (size_t)ly * (IMG_PANEL_W / 2);
        if (syi < 0 || syi >= sh) {
            memset(row, 0x11, IMG_PANEL_W / 2);
            continue;
        }
        const uint8_t *crow = codes + (size_t)syi * (size_t)sw;
        for (int lx = 0; lx < IMG_PANEL_W; lx += 2) {
            int sx0 = (int)(((float)lx + 0.5f - ox) * x_scale);
            int sx1 = (int)(((float)lx + 1.5f - ox) * x_scale);
            uint8_t a = 0x1, b = 0x1;
            if (sx0 >= 0 && sx0 < sw) {
                a = crow[sx0];
            }
            if (sx1 >= 0 && sx1 < sw) {
                b = crow[sx1];
            }
            row[lx / 2] = (uint8_t)((a << 4) | b);
        }
        if ((ly & 63) == 0) {
            kick_wdt();
        }
    }
}

/** Nearest upscale RGB toward budget (QUALITY) so FS runs at higher res. */
static uint8_t *rgb_upscale_toward_budget(uint8_t *rgb, int *w_io, int *h_io, size_t budget)
{
    int sw = *w_io, sh = *h_io;
    if (!rgb || sw < 1 || sh < 1) {
        return rgb;
    }
    size_t cur = (size_t)sw * (size_t)sh * 3;
    if (cur * 2 > budget) {
        return rgb; /* already using most of the budget */
    }
    int scale = 2;
    while ((size_t)sw * scale * (size_t)sh * scale * 3 <= budget && scale < 4) {
        scale++;
    }
    scale--;
    if (scale < 2) {
        return rgb;
    }
    int dw = sw * scale;
    int dh = sh * scale;
    size_t nsz = (size_t)dw * (size_t)dh * 3;
    uint8_t *out = spiram_alloc(nsz);
    if (!out) {
        out = malloc(nsz);
    }
    if (!out) {
        return rgb;
    }
    for (int y = 0; y < dh; y++) {
        int sy = y / scale;
        for (int x = 0; x < dw; x++) {
            int sx = x / scale;
            size_t si = ((size_t)sy * (size_t)sw + (size_t)sx) * 3;
            size_t di = ((size_t)y * (size_t)dw + (size_t)x) * 3;
            out[di] = rgb[si];
            out[di + 1] = rgb[si + 1];
            out[di + 2] = rgb[si + 2];
        }
        if ((y & 31) == 0) {
            kick_wdt();
        }
    }
    free(rgb);
    *w_io = dw;
    *h_io = dh;
    ESP_LOGI(TAG, "quality upscale %dx%d → %dx%d", sw, sh, dw, dh);
    return out;
}

/**
 * URL path: source FS → packed ink codes (caller streams or covers to FB).
 */
static int rgb_to_codes_owned(uint8_t *rgb, int *sw_io, int *sh_io, uint8_t **codes_out,
                              img_proc_mode_t mode)
{
    prep_lut_init();
    *codes_out = NULL;
    int sw = *sw_io, sh = *sh_io;

    if (mode == IMG_PROC_QUALITY) {
        rgb = rgb_upscale_toward_budget(rgb, &sw, &sh, rgb_budget_for(mode));
    }

    size_t n = (size_t)sw * (size_t)sh;
    ESP_LOGI(TAG, "dither %dx%d (%s)", sw, sh,
             mode == IMG_PROC_QUALITY ? "quality" : "speed");
    int64_t t0 = esp_timer_get_time();
    if (dither_rgb_inplace_codes(rgb, sw, sh) != 0) {
        ESP_LOGW(TAG, "FS OOM — nearest only");
        for (size_t i = 0; i < n; i++) {
            int r = rgb[i * 3], g = rgb[i * 3 + 1], b = rgb[i * 3 + 2];
            prep_eink_u8(&r, &g, &b);
            rgb[i] = nearest_ink_prepped(r, g, b);
        }
    }
    uint8_t *codes = realloc(rgb, n);
    if (!codes) {
        codes = rgb;
    }
    ESP_LOGI(TAG, "dither done in %lld ms (codes %u)",
             (long long)((esp_timer_get_time() - t0) / 1000), (unsigned)n);
    *sw_io = sw;
    *sh_io = sh;
    *codes_out = codes;
    return 0;
}

static int rgb_to_panel_fb_owned(uint8_t *rgb, int sw, int sh, uint8_t **fb_out,
                                 img_proc_mode_t mode)
{
    *fb_out = NULL;
    uint8_t *codes = NULL;
    if (rgb_to_codes_owned(rgb, &sw, &sh, &codes, mode) != 0 || !codes) {
        return -1;
    }
    ESP_LOGI(TAG, "cover→panel FB (spiram free %u largest %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    if (panel_fb_ensure() != 0) {
        free(codes);
        return -1;
    }
    codes_cover_to_fb(codes, sw, sh, s_panel_fb);
    free(codes);
    *fb_out = s_panel_fb;
    return 0;
}

int img_rgb_dither_codes(uint8_t *rgb, int w, int h, uint8_t **codes_out, int *cw_out,
                         int *ch_out, img_proc_mode_t mode)
{
    if (!rgb || !codes_out || !cw_out || !ch_out || w < 1 || h < 1) {
        free(rgb);
        return -1;
    }
    *codes_out = NULL;
    int sw = w, sh = h;
    if (rgb_to_codes_owned(rgb, &sw, &sh, codes_out, mode) != 0) {
        return -1;
    }
    *cw_out = sw;
    *ch_out = sh;
    return 0;
}

/* ---------- JPEG ---------- */

typedef struct {
    const uint8_t *data;
    size_t len, pos;
    uint8_t *rgb;
    int tw, th;
} jpeg_io_t;

static UINT jpeg_in(JDEC *jd, BYTE *buff, UINT nbyte)
{
    jpeg_io_t *io = (jpeg_io_t *)jd->device;
    if (!buff) {
        if (io->pos + nbyte > io->len) {
            nbyte = (UINT)(io->len - io->pos);
        }
        io->pos += nbyte;
        return nbyte;
    }
    size_t left = io->len - io->pos;
    if (nbyte > left) {
        nbyte = (UINT)left;
    }
    memcpy(buff, io->data + io->pos, nbyte);
    io->pos += nbyte;
    return nbyte;
}

static UINT jpeg_out_rgb(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_io_t *io = (jpeg_io_t *)jd->device;
    BYTE *src = (BYTE *)bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        for (int x = rect->left; x <= rect->right; x++) {
            size_t o = ((size_t)y * (size_t)io->tw + (size_t)x) * 3;
            io->rgb[o] = *src++;
            io->rgb[o + 1] = *src++;
            io->rgb[o + 2] = *src++;
        }
    }
    return 1;
}

static int decode_jpeg_rgb(const uint8_t *data, size_t len, uint8_t **rgb_out, int *w_out,
                           int *h_out, size_t budget)
{
    jpeg_io_t io = {.data = data, .len = len, .pos = 0};
    JDEC jd;
    const UINT pool_sz = 48 * 1024;
    void *pool = spiram_alloc(pool_sz);
    if (!pool) {
        ESP_LOGE(TAG, "jpeg pool OOM");
        return -1;
    }

    JRESULT jr = jd_prepare(&jd, jpeg_in, pool, pool_sz, &io);
    if (jr != JDR_OK) {
        ESP_LOGE(TAG, "jd_prepare failed (%d)", (int)jr);
        free(pool);
        return -1;
    }

    BYTE scale = 0;
    UINT tw = jd.width, th = jd.height;
    uint8_t *rgb = NULL;
    size_t rgb_sz = 0;
    for (;;) {
        tw = jd.width >> scale;
        th = jd.height >> scale;
        if (tw < 1) {
            tw = 1;
        }
        if (th < 1) {
            th = 1;
        }
        rgb_sz = (size_t)tw * (size_t)th * 3;
        if (rgb_sz <= budget) {
            rgb = spiram_alloc(rgb_sz);
            if (rgb) {
                break;
            }
        }
        if (scale >= 3) {
            ESP_LOGE(TAG, "jpeg rgb OOM (%u bytes)", (unsigned)rgb_sz);
            free(pool);
            return -1;
        }
        scale++;
    }
    memset(rgb, 255, rgb_sz);
    io.rgb = rgb;
    io.tw = (int)tw;
    io.th = (int)th;

    ESP_LOGI(TAG, "JPEG %ux%u → %ux%u (1/%u)", jd.width, jd.height, tw, th, 1u << scale);
    jr = jd_decomp(&jd, jpeg_out_rgb, scale);
    free(pool);
    if (jr != JDR_OK) {
        ESP_LOGE(TAG, "jd_decomp failed (%d)", (int)jr);
        free(rgb);
        return -1;
    }

    *rgb_out = rgb;
    *w_out = (int)tw;
    *h_out = (int)th;
    return 0;
}

/* ---------- PNG ---------- */

typedef struct {
    const uint8_t *data;
    size_t len, pos;
} png_mem_t;

static void png_read_fn(png_structp png_ptr, png_bytep out, png_size_t need)
{
    png_mem_t *m = (png_mem_t *)png_get_io_ptr(png_ptr);
    if (m->pos + need > m->len) {
        png_error(png_ptr, "PNG read past end");
        return;
    }
    memcpy(out, m->data + m->pos, need);
    m->pos += need;
}

static int decode_png_rgb(const uint8_t *data, size_t len, uint8_t **rgb_out, int *w_out,
                          int *h_out, size_t budget)
{
    png_mem_t mem = {.data = data, .len = len, .pos = 0};
    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        return -1;
    }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        return -1;
    }
    if (setjmp(png_jmpbuf(png_ptr))) {
        ESP_LOGE(TAG, "PNG decode error");
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return -1;
    }

    png_set_read_fn(png_ptr, &mem, png_read_fn);
    png_read_info(png_ptr, info_ptr);

    png_uint_32 width = 0, height = 0;
    int bit_depth = 0, color_type = 0, interlace = 0;
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, &interlace, NULL,
                 NULL);
    if (width == 0 || height == 0 || width > 4096 || height > 4096) {
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return -1;
    }
    if (interlace != PNG_INTERLACE_NONE) {
        ESP_LOGE(TAG, "interlaced PNG not supported");
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return -1;
    }

    if (bit_depth == 16) {
        png_set_strip_16(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) {
        png_set_expand_gray_1_2_4_to_8(png_ptr);
    }
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) {
        png_set_tRNS_to_alpha(png_ptr);
    }
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA) {
        png_set_gray_to_rgb(png_ptr);
    }
    if (!(color_type & PNG_COLOR_MASK_ALPHA)) {
        png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);
    }
    png_read_update_info(png_ptr, info_ptr);
    size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);

    int step = 1;
    while (((size_t)(width / step) * (size_t)(height / step) * 3) > budget && step < 32) {
        step++;
    }
    int dw = (int)((width + step - 1) / step);
    int dh = (int)((height + step - 1) / step);
    if (dw < 1) {
        dw = 1;
    }
    if (dh < 1) {
        dh = 1;
    }

    size_t rgb_sz = (size_t)dw * (size_t)dh * 3;
    /* If still too big for the largest block, keep stepping down. */
    while (rgb_sz + rowbytes + 64 > heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) &&
           step < 32) {
        step++;
        dw = (int)((width + step - 1) / step);
        dh = (int)((height + step - 1) / step);
        if (dw < 1) {
            dw = 1;
        }
        if (dh < 1) {
            dh = 1;
        }
        rgb_sz = (size_t)dw * (size_t)dh * 3;
    }
    uint8_t *rgb = spiram_alloc(rgb_sz);
    uint8_t *row = spiram_alloc(rowbytes + 64);
    if (!rgb || !row) {
        ESP_LOGE(TAG, "PNG OOM rgb=%u row=%u free_s=%u largest=%u",
                 (unsigned)rgb_sz, (unsigned)(rowbytes + 64),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        free(rgb);
        free(row);
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        return -1;
    }
    memset(rgb, 255, rgb_sz);

    int bpp = (width > 0) ? (int)(rowbytes / width) : 4;
    if (bpp < 3) {
        bpp = 4;
    }

    ESP_LOGI(TAG, "PNG %lux%lu → %dx%d (step=%d)", (unsigned long)width, (unsigned long)height, dw,
             dh, step);

    int oy = 0;
    for (png_uint_32 y = 0; y < height; y++) {
        png_read_row(png_ptr, row, NULL);
        if ((int)(y % step) != 0) {
            continue;
        }
        if (oy >= dh) {
            continue;
        }
        int ox = 0;
        for (png_uint_32 x = 0; x < width; x += step) {
            if (ox >= dw) {
                break;
            }
            uint8_t *px = row + (size_t)x * (size_t)bpp;
            int r = px[0], g = px[1], b = px[2];
            uint8_t a = (bpp >= 4) ? px[3] : 255;
            if (a != 255) {
                r = (r * a + 255 * (255 - a) + 127) / 255;
                g = (g * a + 255 * (255 - a) + 127) / 255;
                b = (b * a + 255 * (255 - a) + 127) / 255;
            }
            size_t o = ((size_t)oy * (size_t)dw + (size_t)ox) * 3;
            rgb[o] = (uint8_t)r;
            rgb[o + 1] = (uint8_t)g;
            rgb[o + 2] = (uint8_t)b;
            ox++;
        }
        oy++;
        if ((y & 31) == 0) {
            kick_wdt();
        }
    }
    png_read_end(png_ptr, NULL);
    free(row);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

    *rgb_out = rgb;
    *w_out = dw;
    *h_out = oy < dh ? oy : dh;
    return 0;
}

int img_decode_to_rgb(const uint8_t *data, size_t len, uint8_t **rgb_out, int *w_out, int *h_out,
                      img_proc_mode_t mode)
{
    if (!data || !rgb_out || !w_out || !h_out || len == 0) {
        return -1;
    }
    *rgb_out = NULL;
    size_t budget = rgb_budget_for(mode);
    switch (img_sniff_format(data, len)) {
    case IMG_FMT_JPEG:
        return decode_jpeg_rgb(data, len, rgb_out, w_out, h_out, budget);
    case IMG_FMT_PNG:
        return decode_png_rgb(data, len, rgb_out, w_out, h_out, budget);
    case IMG_FMT_WEBP:
        ESP_LOGE(TAG, "WebP not supported on-device (use platform convert)");
        return -1;
    case IMG_FMT_RAW4BPP:
        ESP_LOGE(TAG, "raw 4bpp is not decoded via img_decode_to_rgb");
        return -1;
    default:
        ESP_LOGE(TAG, "unknown image format (%u bytes)", (unsigned)len);
        return -1;
    }
}

int img_rgb_dither_alloc_fb(uint8_t *rgb, int w, int h, uint8_t **fb_out, img_proc_mode_t mode)
{
    if (!fb_out) {
        free(rgb);
        return -1;
    }
    *fb_out = NULL;
    if (!rgb || w < 1 || h < 1) {
        free(rgb);
        return -1;
    }
    return rgb_to_panel_fb_owned(rgb, w, h, fb_out, mode);
}

int img_decode_alloc_fb(const uint8_t *data, size_t len, uint8_t **fb_out, img_proc_mode_t mode)
{
    if (!fb_out) {
        return -1;
    }
    *fb_out = NULL;
    uint8_t *rgb = NULL;
    int w = 0, h = 0;
    if (img_decode_to_rgb(data, len, &rgb, &w, &h, mode) != 0) {
        return -1;
    }
    return img_rgb_dither_alloc_fb(rgb, w, h, fb_out, mode);
}
