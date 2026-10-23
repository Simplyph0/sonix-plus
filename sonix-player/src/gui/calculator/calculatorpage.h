#ifndef GUI_CALCULATORPAGE_H
#define GUI_CALCULATORPAGE_H

#include "src/gui/shell/gui.h"

#include "lvgl/lvgl.h"

// The Calculator tile in More: the four operations, percent and sign, with a
// pill in the corner that adds the scientific keys above them -- brackets,
// memory, powers and roots, logarithms, trigonometry in degrees or radians,
// factorial, pi and e -- the way a phone's calculator does when it is turned
// on its side. The player is always upright, so it is a switch instead.
//
// Whether the scientific keys are shown, and degrees or radians, are kept in the
// config, [calculator] scientific and degrees. The sums themselves are in
// system/calc.
extern lv_obj_t *calculatorpage_screen;

void calculatorpage_init(gui_config_t *cfg);

#endif /* GUI_CALCULATORPAGE_H */
