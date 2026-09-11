#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize the screen: mount SD card, set up LVGL UI, start GIF playback.
void screen_init(void);

/// Register a callback invoked on screen tap (e.g. to reset inactivity timer).
void screen_set_tap_cb(void (*cb)(void));

/// Show text overlay on the screen.
void screen_show_text(const char *text);

/// Trigger the next random GIF. Ignored while one is already playing.
void screen_next_gif(void);

#ifdef __cplusplus
}
#endif
