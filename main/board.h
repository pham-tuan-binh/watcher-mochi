#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize board.
void board_init(void);

/// Register a callback for when the knob button is pressed down.
void board_set_btn_press_cb(void (*cb)(void));

/// Register a callback for when the knob button is released.
void board_set_btn_release_cb(void (*cb)(void));

/// Register a callback for when the knob button is long-pressed.
void board_set_btn_long_press_cb(void (*cb)(void));

/// Enter deep sleep. Wakes on button press (or after time_sec seconds if > 0).
void board_deep_sleep(uint32_t time_sec);

/// Set LCD backlight brightness (0–100%).
void board_set_lcd_brightness(int percent);

/// Play a short pop sound through the speaker.
void board_play_pop(void);

#ifdef __cplusplus
}
#endif
