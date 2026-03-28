#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize the screen UI.
void screen_init(void);

/// Register a callback invoked on screen tap (e.g. to reset inactivity timer).
void screen_set_tap_cb(void (*cb)(void));

/// Show text on the screen, replacing any previous text.
void screen_show_text(const char *text);

/// Switch to the next random GIF. Ignored while a GIF is already playing.
void screen_next_gif(void);

#ifdef __cplusplus
}
#endif
