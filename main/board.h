#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize board hardware (IO expander, codec, touch, display).
void board_init(void);

/// Register a callback for knob button press.
void board_set_btn_press_cb(void (*cb)(void));

/// Register a callback for knob button release.
void board_set_btn_release_cb(void (*cb)(void));

/// Register a callback for knob button long-press release.
void board_set_btn_long_press_cb(void (*cb)(void));

/// Enter deep sleep. Wakes on button press (or after time_sec seconds if > 0).
void board_deep_sleep(uint32_t time_sec);

/// Set LCD backlight brightness (0-100%).
void board_set_lcd_brightness(int percent);

#ifdef __cplusplus
}
#endif
