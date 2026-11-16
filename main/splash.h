/**
 * @file splash.h
 * @brief Default boot splash — swappable asset, centered on landscape panel.
 *
 * Later splash redesigns should only replace the asset (or add a new
 * splash_asset_t) and call splash_show(); keep compositing here.
 */
#ifndef __SPLASH_H__
#define __SPLASH_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const unsigned char *bits; /* 4bpp packed, row-major, even width */
    int w;
    int h;
    unsigned char bg; /* panel fill, e.g. BLACK 0x00 */
} splash_asset_t;

/* Built-in CamThink wordmark (e-ink remapped). */
const splash_asset_t *splash_default_asset(void);

/* Sync: black (or asset.bg) full panel + centered bitmap + refresh. */
void splash_show(const splash_asset_t *asset);

/* Sync default CamThink splash (no caption text). */
void splash_show_default(void);

/* Queue default splash on the epd render task. */
int splash_show_default_async(void);

#ifdef __cplusplus
}
#endif

#endif /* __SPLASH_H__ */
