#ifndef PANEL_H
#define PANEL_H

#include <stdbool.h>

// Who owns the touchscreen.
//
// Declared here, defined in main.c -- that is where touch is opened -- with an
// empty version for the host build. Same shape as display_rotation_supported()
// and friends in power.h, which live there for the same reason (and
// display_get_rotated() is what a caller reading the panel itself needs:
// rotation has to be applied by hand, or the controls come out upside down).
//
// These exist for the emulator. A Game Boy is held with two thumbs and LVGL is
// not: its evdev indev is a pointer, a single contact, which cannot press the
// d-pad and A/B together. So while a game runs the panel changes owner: LVGL's
// indev is switched off, the emulator opens the same evdev node itself and
// reads the real multitouch protocol; on exit everything goes back.

// Switches LVGL's touch indev on and off. Off, LVGL does not read the node and
// moves nothing: the panel is free.
void panel_touch_enable(bool enabled);

// The evdev node LVGL's indev opened: the one that reports itself as a touch
// panel, or event1 and then event0 when none does. NULL when there is no touch
// device.
const char *panel_touch_device(void);

// ---------------------------------------------------------------------------
// Handing the whole panel to a program of its own
// ---------------------------------------------------------------------------
//
// An app from the card (system/apps) draws on the framebuffer itself and reads
// the touch panel itself. While one runs the player must neither draw nor read:
// panel_app_begin() stops LVGL painting and parks its touch indev, and
// panel_app_end() puts the panel back the way the player had it and repaints
// every page of the interface. On the host build they do nothing.
void panel_app_begin(void);
void panel_app_end(void);

// What an app needs to know to draw: the panel's size and whether the picture
// is turned 180 degrees (Settings > Display), which the app has to do for
// itself on the framebuffer and on the touch coordinates. False without a
// framebuffer (the host build).
bool panel_app_geometry(int *width, int *height, bool *rotated);

#endif /* PANEL_H */
