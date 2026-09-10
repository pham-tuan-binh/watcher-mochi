#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Create the koi pond on `parent` and start animating it.
void pond_init(lv_obj_t *parent);

/// Drop a ripple at a screen coordinate. The koi swim over to investigate.
void pond_tap(lv_coord_t x, lv_coord_t y);

#ifdef __cplusplus
}
#endif
