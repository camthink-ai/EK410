/**
 * @file canvas.c
 * @brief Canvas draw-list: JSON parse + per-pixel evaluation + QR encoding.
 *
 * Rendering is per-pixel over logical landscape 1600x1200 (C61 has no full
 * framebuffer). Text supports UTF-8 ASCII (8x16) + CJK (16x16 GB2312 set).
 * QR payloads (max 2 per canvas) are encoded up-front with Nayuki qrcodegen.
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "esp_log.h"
#include "cJSON.h"
#include "qrcodegen.h"

#include "canvas.h"
#include "app_epd.h"
#include "font8x16.h"
#include "font_hz16.h"
#include "EPD709.h"

#define TAG "-->CVS"

/* QR payloads are encoded into cv->qr[] (inside the canvas struct, copied
 * with the render message), temp scratch is per-call. */
static uint8_t s_qrTemp[qrcodegen_BUFFER_LEN_MAX];

static char ascii_fallback_cp(uint32_t cp);
static int cell_w_for_cp(uint32_t cp);
static int utf8_decode(const char *s, int *advance, uint32_t *cp);

/* Copy UTF-8 without splitting a multibyte sequence (strlcpy can truncate mid-glyph). */
void canvas_utf8_strlcpy(char *dst, const char *src, size_t dstSz)
{
    if (!dst || dstSz == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    while (src[i] && i + 1 < dstSz) {
        int adv = 1;
        uint32_t cp = 0;
        utf8_decode(src + i, &adv, &cp);
        if (i + (size_t)adv >= dstSz) {
            break;
        }
        for (int k = 0; k < adv; k++) {
            dst[i + (size_t)k] = src[i + (size_t)k];
        }
        i += (size_t)adv;
    }
    dst[i] = '\0';
}

unsigned char canvas_color(const char *name)
{
    if (!name) return 0x00;
    if (!strcasecmp(name, "white"))  return 0x11;
    if (!strcasecmp(name, "yellow")) return 0x22;
    if (!strcasecmp(name, "red"))    return 0x33;
    if (!strcasecmp(name, "blue"))   return 0x55;
    if (!strcasecmp(name, "green"))  return 0x66;
    return 0x00;
}

static int json_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static bool json_color(const cJSON *obj, const char *key, unsigned char *out)
{
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(v)) {
        *out = canvas_color(v->valuestring);
        return true;
    }
    return false;
}

int canvas_parse_json(const cJSON *root, canvas_t *cv, char *errBuf, int errLen)
{
    memset(cv, 0, sizeof(*cv));
    cv->bg = 0x11;
    json_color(root, "bg", &cv->bg);

    int qrSlot = 0;
    const cJSON *items = cJSON_GetObjectItem(root, "items");
    if (!cJSON_IsArray(items)) {
        snprintf(errBuf, errLen, "missing items[]");
        return -1;
    }

    const cJSON *it;
    cJSON_ArrayForEach(it, items) {
        if (cv->count >= CANVAS_MAX_ITEMS) {
            break;
        }
        const cJSON *type = cJSON_GetObjectItem(it, "type");
        if (!cJSON_IsString(type)) {
            continue;
        }
        canvas_item_t *item = &cv->items[cv->count];
        item->x = json_int(it, "x", 0);
        item->y = json_int(it, "y", 0);
        if (!json_color(it, "color", &item->color)) {
            item->color = 0x00;
        }

        if (!strcasecmp(type->valuestring, "text")) {
            const cJSON *t = cJSON_GetObjectItem(it, "text");
            if (!cJSON_IsString(t)) {
                continue;
            }
            item->type = CI_TEXT;
            canvas_utf8_strlcpy(item->text, t->valuestring, sizeof(item->text));
            item->scale = json_int(it, "scale", 3);
            if (item->scale < 1) item->scale = 1;
            if (item->scale > 8) item->scale = 8;
            /* UTF-8 advance: ASCII / punctuation-fallback 8px, CJK/emoji cell 16px. */
            int lines = 1, maxAdv = 0, adv = 0;
            for (int i = 0; item->text[i];) {
                if (item->text[i] == '\n') {
                    if (adv > maxAdv) {
                        maxAdv = adv;
                    }
                    lines++;
                    adv = 0;
                    i++;
                    continue;
                }
                int step = 1;
                uint32_t cp = 0;
                utf8_decode(item->text + i, &step, &cp);
                adv += cell_w_for_cp(cp);
                i += step;
            }
            if (adv > maxAdv) {
                maxAdv = adv;
            }
            item->w = maxAdv * item->scale;
            item->h = lines * (HZ16_H * item->scale + item->scale);
        } else if (!strcasecmp(type->valuestring, "rect")) {
            item->type = CI_RECT;
            item->w = json_int(it, "w", 100);
            item->h = json_int(it, "h", 100);
            item->thickness = json_int(it, "thickness", 4);
            const cJSON *f = cJSON_GetObjectItem(it, "fill");
            item->fill = cJSON_IsBool(f) ? cJSON_IsTrue(f) : false;
        } else if (!strcasecmp(type->valuestring, "qr")) {
            if (qrSlot >= CANVAS_QR_SLOTS) {
                snprintf(errBuf, errLen, "max %d qr items", CANVAS_QR_SLOTS);
                return -1;
            }
            const cJSON *t = cJSON_GetObjectItem(it, "text");
            if (!cJSON_IsString(t)) {
                continue;
            }
            if (!qrcodegen_encodeText(t->valuestring, s_qrTemp,
                                      cv->qr[qrSlot], qrcodegen_Ecc_MEDIUM,
                                      qrcodegen_VERSION_MIN, CANVAS_QR_VERSION_MAX,
                                      qrcodegen_Ecc_MEDIUM, false)) {
                snprintf(errBuf, errLen, "qr encode failed (payload > v%d?)", CANVAS_QR_VERSION_MAX);
                return -1;
            }
            item->type = CI_QR;
            canvas_utf8_strlcpy(item->text, t->valuestring, sizeof(item->text)); /* keep payload for logs */
            item->qrSlot = qrSlot;
            item->qrSize = qrcodegen_getSize(cv->qr[qrSlot]);
            item->scale = json_int(it, "scale", 6);
            if (item->scale < 2) item->scale = 2;
            if (item->scale > 16) item->scale = 16;
            qrSlot++;
        } else {
            snprintf(errBuf, errLen, "unknown item type");
            return -1;
        }
        cv->count++;
    }
    ESP_LOGI(TAG, "canvas parsed: %d items", cv->count);
    return 0;
}

/* ---------- per-pixel evaluation ---------- */

static unsigned char ascii_glyph_bit(char c, int row, int col)
{
    if (c < FONT_FIRST || c > FONT_LAST) {
        c = '?';
    }
    const unsigned char *g = font8x16[c - FONT_FIRST];
    return (g[row] >> (FONT_W - 1 - col)) & 1;
}

/* Map common Unicode punctuation (often pasted from Word/IME) to ASCII so
 * dashes / quotes / bullets render instead of hollow boxes. */
static char ascii_fallback_cp(uint32_t cp)
{
    if (cp >= 0xFF01 && cp <= 0xFF5E) {
        return (char)(cp - 0xFEE0); /* fullwidth ASCII */
    }
    switch (cp) {
    case 0x2010: case 0x2011: case 0x2012: case 0x2013:
    case 0x2014: case 0x2015: case 0x2212: case 0xFE58:
    case 0xFE63: case 0xFF0D:
        return '-';
    case 0x2018: case 0x2019: case 0x201A: case 0x2032:
        return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033:
        return '"';
    case 0x2022: case 0x00B7: case 0x2023: case 0x2043:
    case 0x30FB: /* ・ */
        return '*';
    case 0x2026:
        return '.';
    case 0x00A0: case 0x3000:
        return ' ';
    case 0x00D7: case 0x2715: case 0x2716: /* × ✕ ✖ */
        return 'x';
    case 0x00F7: /* ÷ */
        return '/';
    case 0x2190: return '<';
    case 0x2192: return '>';
    case 0x2191: return '^';
    case 0x2193: return 'v';
    default:
        return 0;
    }
}

static int utf8_decode(const char *s, int *advance, uint32_t *cp)
{
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) {
        *cp = p[0];
        *advance = 1;
        return 0;
    }
    if ((p[0] & 0xE0) == 0xC0 && p[1]) {
        *cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        *advance = 2;
        return 0;
    }
    if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        *cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        *advance = 3;
        return 0;
    }
    if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
        *cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
              ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
        *advance = 4;
        return 0;
    }
    *cp = '?';
    *advance = 1;
    return -1;
}

static int cell_w_for_cp(uint32_t cp)
{
    if (cp < 0x80 || ascii_fallback_cp(cp)) {
        return FONT_W;
    }
    return HZ16_W;
}

/* multiline UTF-8 text; bbox precomputed at parse time */
static bool text_pixel(const canvas_item_t *it, int x, int y)
{
    if (x < it->x || y < it->y || x >= it->x + it->w || y >= it->y + it->h) {
        return false;
    }
    int lineH = HZ16_H * it->scale + it->scale;
    int line = (y - it->y) / lineH;
    int py = (y - it->y) % lineH;
    if (py >= HZ16_H * it->scale) {
        return false;
    }
    int gy = py / it->scale;

    /* seek to line */
    int idx = 0, cur = 0;
    while (cur < line && it->text[idx]) {
        if (it->text[idx] == '\n') {
            cur++;
            idx++;
        } else {
            int adv = 1;
            uint32_t cp;
            utf8_decode(it->text + idx, &adv, &cp);
            idx += adv;
        }
    }
    if (!it->text[idx] || it->text[idx] == '\n') {
        return false;
    }

    int px = x - it->x;
    int cursor = 0;
    while (it->text[idx] && it->text[idx] != '\n') {
        int adv = 1;
        uint32_t cp = 0;
        utf8_decode(it->text + idx, &adv, &cp);
        int cw = cell_w_for_cp(cp);
        int cellPx = cw * it->scale;
        if (px >= cursor && px < cursor + cellPx) {
            int gx = (px - cursor) / it->scale;
            char fb = (cp < 0x80) ? (char)cp : ascii_fallback_cp(cp);
            if (fb) {
                if (gy >= FONT_H || gx >= FONT_W) {
                    return false;
                }
                return ascii_glyph_bit(fb, gy, gx) != 0;
            }
            /* Panel font is BMP GB2312 only — emoji / rare CJK / non-BMP → box. */
            uint8_t bits[32];
            bool have = (cp <= 0xFFFF) && hz16_lookup((uint16_t)cp, bits);
            if (!have) {
                /* Visible 16x16 outline so missing glyphs are obvious, not a speck. */
                return (gx == 0 || gy == 0 || gx == HZ16_W - 1 || gy == HZ16_H - 1);
            }
            if (gx < 0 || gx >= HZ16_W || gy < 0 || gy >= HZ16_H) {
                return false;
            }
            uint16_t row = ((uint16_t)bits[gy * 2] << 8) | bits[gy * 2 + 1];
            return (row & (0x8000 >> gx)) != 0;
        }
        cursor += cellPx;
        idx += adv;
    }
    return false;
}

static bool rect_pixel(const canvas_item_t *it, int x, int y)
{
    int dx = x - it->x, dy = y - it->y;
    if (dx < 0 || dy < 0 || dx >= it->w || dy >= it->h) {
        return false;
    }
    if (it->fill) {
        return true;
    }
    int t = it->thickness;
    return (dx < t || dy < t || dx >= it->w - t || dy >= it->h - t);
}

static bool qr_pixel(const canvas_t *cv, const canvas_item_t *it, int x, int y)
{
    int dx = x - it->x, dy = y - it->y;
    int px = dx / it->scale, py = dy / it->scale;
    if (px < 0 || py < 0 || px >= it->qrSize || py >= it->qrSize) {
        return false;
    }
    return qrcodegen_getModule(cv->qr[it->qrSlot], px, py);
}

static unsigned char canvas_pixel(int x, int y, void *ctx)
{
    const canvas_t *cv = (const canvas_t *)ctx;
    unsigned char c = cv->bg;
    for (int i = 0; i < cv->count; i++) {
        const canvas_item_t *it = &cv->items[i];
        bool hit = false;
        switch (it->type) {
        case CI_TEXT:
            hit = text_pixel(it, x, y);
            break;
        case CI_RECT:
            hit = rect_pixel(it, x, y);
            break;
        case CI_QR:
            hit = qr_pixel(cv, it, x, y);
            break;
        }
        if (hit) {
            c = it->color;
        }
    }
    return c;
}

void canvas_render(const canvas_t *cv)
{
    epd_stream_render(canvas_pixel, (void *)cv);
}

static int setup_add_text(canvas_t *cv, int x, int y, int scale, const char *text)
{
    if (cv->count >= CANVAS_MAX_ITEMS || !text) {
        return -1;
    }
    if (scale < 1) {
        scale = 1;
    }
    if (scale > 8) {
        scale = 8;
    }
    canvas_item_t *it = &cv->items[cv->count++];
    memset(it, 0, sizeof(*it));
    it->type = CI_TEXT;
    it->x = x;
    it->y = y;
    it->scale = scale;
    it->color = 0x00; /* black */
    canvas_utf8_strlcpy(it->text, text, sizeof(it->text));
    /* Approximate bounds for hit-testing / layout (ASCII-only setup copy). */
    it->w = (int)strlen(it->text) * FONT_W * scale;
    it->h = FONT_H * scale;
    return 0;
}

int canvas_fill_text_screen(canvas_t *cv, const char *const *lines, int line_count,
                            unsigned char fg, unsigned char bg, int scale)
{
    if (!cv) {
        return -1;
    }
    memset(cv, 0, sizeof(*cv));
    cv->bg = bg;
    if (scale < 1) {
        scale = 1;
    }
    if (scale > 8) {
        scale = 8;
    }
    if (line_count < 0) {
        line_count = 0;
    }
    if (line_count > CANVAS_MAX_ITEMS) {
        line_count = CANVAS_MAX_ITEMS;
    }

    int line_h = HZ16_H * scale + scale;
    int total_h = line_h * line_count;
    int top = (EPD_PANEL_H - total_h) / 2;
    if (top < 40) {
        top = 40;
    }

    for (int i = 0; i < line_count; i++) {
        if (!lines[i] || !lines[i][0]) {
            continue;
        }
        if (cv->count >= CANVAS_MAX_ITEMS) {
            break;
        }
        canvas_item_t *it = &cv->items[cv->count++];
        memset(it, 0, sizeof(*it));
        it->type = CI_TEXT;
        it->scale = scale;
        it->color = fg;
        canvas_utf8_strlcpy(it->text, lines[i], sizeof(it->text));

        int adv = 0;
        for (int j = 0; it->text[j];) {
            int step = 1;
            uint32_t cp = 0;
            utf8_decode(it->text + j, &step, &cp);
            adv += cell_w_for_cp(cp);
            j += step;
        }
        it->w = adv * scale;
        it->h = HZ16_H * scale;
        it->x = (EPD_PANEL_W - it->w) / 2;
        if (it->x < 0) {
            it->x = 0;
        }
        it->y = top + i * line_h;
    }
    return 0;
}

int canvas_fill_setup_screen(canvas_t *cv, const char *ssid)
{
    if (!cv) {
        return -1;
    }
    memset(cv, 0, sizeof(*cv));
    cv->bg = 0x11; /* white */

    const char *ap = (ssid && ssid[0]) ? ssid : "EK410";
    char lineSsid[80];
    char linePass[48];
    snprintf(lineSsid, sizeof(lineSsid), "%s", ap);
    snprintf(linePass, sizeof(linePass), "Password  12345678");

    /* Left column — large readable steps (landscape 1600x1200). */
    setup_add_text(cv, 72, 64, 6, "SETUP");
    setup_add_text(cv, 72, 200, 3, "1. Join Wi-Fi");
    setup_add_text(cv, 72, 280, 4, lineSsid);
    setup_add_text(cv, 72, 380, 4, linePass);
    setup_add_text(cv, 72, 520, 3, "2. Open setup page");
    setup_add_text(cv, 72, 600, 3, "http://192.168.4.1");
    setup_add_text(cv, 72, 720, 2, "or scan the QR code");

    /* Right — URL QR (works after the phone joins SoftAP). */
    static const char *kSetupUrl = "http://192.168.4.1";
    if (!qrcodegen_encodeText(kSetupUrl, s_qrTemp, cv->qr[0], qrcodegen_Ecc_MEDIUM,
                              qrcodegen_VERSION_MIN, CANVAS_QR_VERSION_MAX,
                              qrcodegen_Mask_AUTO, true)) {
        ESP_LOGE(TAG, "setup QR encode failed");
        return -1;
    }
    int qrSize = qrcodegen_getSize(cv->qr[0]);
    /* Target ~420px modules on the right half. */
    int qrScale = 420 / (qrSize > 0 ? qrSize : 1);
    if (qrScale < 8) {
        qrScale = 8;
    }
    if (qrScale > 16) {
        qrScale = 16;
    }
    int qrPx = qrSize * qrScale;
    int qrX = 1600 - 72 - qrPx;
    int qrY = 220;
    if (cv->count >= CANVAS_MAX_ITEMS) {
        return -1;
    }
    canvas_item_t *qr = &cv->items[cv->count++];
    memset(qr, 0, sizeof(*qr));
    qr->type = CI_QR;
    qr->x = qrX;
    qr->y = qrY;
    qr->color = 0x00;
    qr->qrSlot = 0;
    qr->qrSize = qrSize;
    qr->scale = qrScale;
    canvas_utf8_strlcpy(qr->text, kSetupUrl, sizeof(qr->text));

    setup_add_text(cv, qrX, qrY + qrPx + 24, 2, "Scan after joining Wi-Fi");
    ESP_LOGI(TAG, "setup screen: ssid=%s qr=%dx%d scale=%d", ap, qrSize, qrSize, qrScale);
    return 0;
}
