/**
 * @file splash.c
 * @brief Centered splash compositor over logical landscape 1600x1200.
 */
#include "splash.h"
#include "splash_logo_data.h"
#include "app_epd.h"
#include "EPD709.h"
#include "esp_log.h"

#define TAG "-->SPLASH"

static const splash_asset_t s_default = {
    .bits = SPLASH_LOGO_BITS,
    .w = SPLASH_LOGO_W,
    .h = SPLASH_LOGO_H,
    .bg = WHITE, /* black wordmark on white panel */
};

const splash_asset_t *splash_default_asset(void)
{
    return &s_default;
}

typedef struct {
    const splash_asset_t *asset;
    int x0;
    int y0;
} splash_ctx_t;

static unsigned char splash_pixel(int x, int y, void *ctx)
{
    const splash_ctx_t *c = (const splash_ctx_t *)ctx;
    const splash_asset_t *a = c->asset;
    if (x < c->x0 || y < c->y0 || x >= c->x0 + a->w || y >= c->y0 + a->h) {
        return a->bg;
    }
    int lx = x - c->x0;
    int ly = y - c->y0;
    unsigned char byte = a->bits[ly * (a->w / 2) + (lx / 2)];
    return (lx & 1) ? (byte & 0x0F) : (byte >> 4);
}

void splash_show(const splash_asset_t *asset)
{
    if (!asset || !asset->bits || asset->w < 2 || (asset->w & 1) || asset->h < 1) {
        ESP_LOGE(TAG, "invalid splash asset");
        return;
    }
    splash_ctx_t ctx = {
        .asset = asset,
        .x0 = (EPD_PANEL_W - asset->w) / 2,
        .y0 = (EPD_PANEL_H - asset->h) / 2,
    };
    if (ctx.x0 < 0) {
        ctx.x0 = 0;
    }
    if (ctx.y0 < 0) {
        ctx.y0 = 0;
    }
    ESP_LOGI(TAG, "show %dx%d at (%d,%d) bg=0x%02X",
             asset->w, asset->h, ctx.x0, ctx.y0, asset->bg);
    epd_stream_render(splash_pixel, &ctx);
}

void splash_show_default(void)
{
    splash_show(&s_default);
}

int splash_show_default_async(void)
{
    return app_epd_splash_async();
}
