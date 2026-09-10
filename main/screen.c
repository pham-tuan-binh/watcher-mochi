#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "sensecap-watcher.h"
#include "board.h"
#include "pond.h"
#include "screen.h"
#include "sound.h"

static const char *TAG = "screen";

static void (*s_tap_cb)(void);

/// Give LVGL a frame or two to push the first pond render out to the panel
/// before the backlight comes up, so the display never flashes garbage.
static void backlight_cb(lv_timer_t *t)
{
    board_set_lcd_brightness(100);
    lv_timer_del(t);
}

static void screen_press_cb(lv_event_t *e)
{
    (void)e;

    lv_point_t p = { DRV_LCD_H_RES / 2, DRV_LCD_V_RES / 2 };
    lv_indev_t *indev = lv_indev_get_act();
    if (indev && lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER)
        lv_indev_get_point(indev, &p);

    sound_play_pop();
    pond_tap(p.x, p.y);

    if (s_tap_cb)
        s_tap_cb();
}

void screen_init(void)
{
    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, screen_press_cb, LV_EVENT_PRESSED, NULL);

    pond_init(scr);
    lv_timer_create(backlight_cb, 200, NULL);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Screen ready");
}

void screen_set_tap_cb(void (*cb)(void))
{
    s_tap_cb = cb;
}
