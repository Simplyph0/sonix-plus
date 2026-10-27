#include "gamecanvas.h"

#include <stdlib.h>
#include <string.h>

#include "lvgl/src/misc/cache/instance/lv_image_cache.h"

bool gamecanvas_create(gamecanvas_t *c, lv_obj_t *parent, int x, int y, int w, int h, uint16_t color) {
	memset(c, 0, sizeof(*c));
	c->px = malloc((size_t)w * h * sizeof(uint16_t));
	if (!c->px) {
		return false;
	}
	c->w = w;
	c->h = h;
	gamecanvas_fill(c, color);

	c->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	c->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
	c->dsc.header.w = (uint32_t)w;
	c->dsc.header.h = (uint32_t)h;
	c->dsc.header.stride = (uint32_t)w * 2;
	c->dsc.data_size = (uint32_t)w * (uint32_t)h * 2;
	c->dsc.data = (const uint8_t *)c->px;

	c->image = lv_image_create(parent);
	lv_obj_set_pos(c->image, x, y);
	lv_obj_set_size(c->image, w, h);
	lv_obj_set_clickable(c->image, false);
	lv_image_set_src(c->image, &c->dsc);
	return true;
}

void gamecanvas_destroy(gamecanvas_t *c) {
	if (c->image) {
		lv_image_set_src(c->image, NULL);
		lv_obj_delete(c->image);
		c->image = NULL;
	}
	if (c->px) {
		// LVGL's image cache is keyed by the descriptor's address.
		lv_image_cache_drop(&c->dsc);
		free(c->px);
		c->px = NULL;
	}
	c->w = c->h = 0;
}

void gamecanvas_fill(gamecanvas_t *c, uint16_t color) {
	size_t n = (size_t)c->w * c->h;
	for (size_t i = 0; i < n; i++) {
		c->px[i] = color;
	}
}

void gamecanvas_rect(gamecanvas_t *c, int x, int y, int w, int h, uint16_t color) {
	int x0 = x < 0 ? 0 : x;
	int y0 = y < 0 ? 0 : y;
	int x1 = x + w > c->w ? c->w : x + w;
	int y1 = y + h > c->h ? c->h : y + h;
	for (int yy = y0; yy < y1; yy++) {
		uint16_t *row = c->px + (size_t)yy * c->w;
		for (int xx = x0; xx < x1; xx++) {
			row[xx] = color;
		}
	}
}

void gamecanvas_round_rect(gamecanvas_t *c, int x, int y, int w, int h, int r, uint16_t color) {
	if (r * 2 > w) {
		r = w / 2;
	}
	if (r * 2 > h) {
		r = h / 2;
	}
	for (int yy = 0; yy < h; yy++) {
		int py = y + yy;
		if (py < 0 || py >= c->h) {
			continue;
		}
		// How far the corner cuts into this row.
		int inset = 0;
		if (yy < r) {
			int dy = r - yy - 1;
			// the smallest inset whose point is inside the circle of radius r
			for (inset = 0; inset < r; inset++) {
				int dx = r - inset - 1;
				if (dx * dx + dy * dy <= r * r) {
					break;
				}
			}
		} else if (yy >= h - r) {
			int dy = yy - (h - r);
			for (inset = 0; inset < r; inset++) {
				int dx = r - inset - 1;
				if (dx * dx + dy * dy <= r * r) {
					break;
				}
			}
		}
		int x0 = x + inset;
		int x1 = x + w - inset;
		if (x0 < 0) {
			x0 = 0;
		}
		if (x1 > c->w) {
			x1 = c->w;
		}
		uint16_t *row = c->px + (size_t)py * c->w;
		for (int xx = x0; xx < x1; xx++) {
			row[xx] = color;
		}
	}
}

void gamecanvas_circle(gamecanvas_t *c, int cx, int cy, int r, uint16_t color) {
	gamecanvas_round_rect(c, cx - r, cy - r, r * 2, r * 2, r, color);
}

void gamecanvas_dirty(gamecanvas_t *c, int x, int y, int w, int h) {
	if (!c->image) {
		return;
	}
	// LVGL takes the area in screen coordinates and cuts it to the image.
	lv_area_t a;
	int ox = lv_obj_get_x(c->image);
	int oy = lv_obj_get_y(c->image);
	a.x1 = ox + x;
	a.y1 = oy + y;
	a.x2 = ox + x + w - 1;
	a.y2 = oy + y + h - 1;
	lv_obj_invalidate_area(c->image, &a);
}

void gamecanvas_dirty_all(gamecanvas_t *c) {
	if (c->image) {
		lv_obj_invalidate(c->image);
	}
}

uint16_t gc_mix(uint16_t under, uint16_t over, unsigned opa) {
	unsigned r = (((over >> 11) & 31) * opa + ((under >> 11) & 31) * (255 - opa)) / 255;
	unsigned g = (((over >> 5) & 63) * opa + ((under >> 5) & 63) * (255 - opa)) / 255;
	unsigned b = ((over & 31) * opa + (under & 31) * (255 - opa)) / 255;
	return (uint16_t)((r << 11) | (g << 5) | b);
}
