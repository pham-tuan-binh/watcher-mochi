#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// Set up the LVGL UI and start the koi pond.
void screen_init(void);

/// Register a callback invoked on screen tap (e.g. to reset inactivity timer).
void screen_set_tap_cb(void (*cb)(void));

#ifdef __cplusplus
}
#endif
