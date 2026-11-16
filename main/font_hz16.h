#ifndef FONT_HZ16_H
#define FONT_HZ16_H
#include <stdint.h>
#include <stdbool.h>

#define HZ16_W 16
#define HZ16_H 16

#ifdef __cplusplus
extern "C" {
#endif

int hz16_glyph_count(void);
/* Binary-search Unicode; fills 32-byte glyph (16 rows x 2 bytes MSB-left). Returns false if missing. */
bool hz16_lookup(uint16_t unicode, uint8_t out[32]);

#ifdef __cplusplus
}
#endif
#endif
