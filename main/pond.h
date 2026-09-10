#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Hard limits on how much the pond will simulate at once.
#define POND_MAX_KOI   8
#define POND_MAX_PADS  12
#define POND_MAX_MOTES 16

/// Create the koi pond on `parent` and start animating it.
void pond_init(lv_obj_t *parent);

/// Drop a ripple at a screen coordinate. The koi swim over to investigate.
void pond_tap(lv_coord_t x, lv_coord_t y);

/// Step the zoom by `delta` detents (positive zooms in). Zoomed in, the view
/// follows a koi. Returns true if the zoom actually changed.
bool pond_zoom(int delta);

/// Change how much is simulated. Counts are clamped to the POND_MAX_* limits
/// above; new koi, pads and motes are dropped into the pond and surplus ones
/// are removed. This is how many exist, not how many you can see — they roam
/// past the rim into the dark, so the number on screen drifts either side.
void pond_set_population(int koi, int pads, int motes);

/// Read back what is currently being simulated. Any pointer may be NULL.
void pond_get_population(int *koi, int *pads, int *motes);

#ifdef __cplusplus
}
#endif
