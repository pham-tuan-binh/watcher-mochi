#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// Set up the LVGL UI and start the koi pond.
void screen_init(void);

/// Register a callback invoked on screen tap (e.g. to reset inactivity timer).
void screen_set_tap_cb(void (*cb)(void));

/// Report a knob detent (+1 clockwise, -1 anti) to zoom the pond. Safe to
/// call from any task; the zoom is applied from the LVGL task.
void screen_knob(int dir);

#ifdef __cplusplus
}
#endif
