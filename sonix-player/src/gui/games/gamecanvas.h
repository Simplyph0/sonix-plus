#ifndef GUI_GAMECANVAS_H
#define GUI_GAMECANVAS_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

// A rectangle of pixels the games draw into themselves, and LVGL shows as an
// image: the way Flappy Bird does it, because a game redrawing a few small things
// sixty times a second does not want a widget for each of them.
//
// Colours are RGB565, drawing is clipped to the canvas, and nothing reaches the
// panel until the game says which part changed (gamecanvas_dirty): only that part
// is repainted.

typedef struct {
	int w;
	int h;
	uint16_t *px;
	lv_image_dsc_t dsc;
	lv_obj_t *image;
} gamecanvas_t;

static inline uint16_t gc_rgb(uint8_t r, uint8_t g, uint8_t b) {
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Makes a w x h canvas at (x, y) of `parent`, filled with `color`. False when the
// memory is not there.
bool gamecanvas_create(gamecanvas_t *c, lv_obj_t *parent, int x, int y, int w, int h, uint16_t color);

// Takes the canvas off the screen and gives its memory back. Safe on one that
// was never created.
void gamecanvas_destroy(gamecanvas_t *c);

void gamecanvas_fill(gamecanvas_t *c, uint16_t color);
void gamecanvas_rect(gamecanvas_t *c, int x, int y, int w, int h, uint16_t color);
// A rectangle with rounded corners of radius `r`.
void gamecanvas_round_rect(gamecanvas_t *c, int x, int y, int w, int h, int r, uint16_t color);
void gamecanvas_circle(gamecanvas_t *c, int cx, int cy, int r, uint16_t color);

// Marks a part of the canvas as changed, to be repainted on the next refresh.
void gamecanvas_dirty(gamecanvas_t *c, int x, int y, int w, int h);
void gamecanvas_dirty_all(gamecanvas_t *c);

// Mixes `over` onto `under`, `opa` of 255 being all of `over`.
uint16_t gc_mix(uint16_t under, uint16_t over, unsigned opa);

#endif /* GUI_GAMECANVAS_H */
