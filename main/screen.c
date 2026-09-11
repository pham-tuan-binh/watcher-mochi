#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "lvgl.h"
#include "sensecap-watcher.h"
#include "board.h"
#include "gif_loader.h"
#include "sound.h"
#include "screen.h"

static const char *TAG = "screen";

#define IDLE_AUTO_PLAY_MS (CONFIG_MOCHI_IDLE_AUTO_PLAY_SEC * 1000)

// --- LVGL widgets ---
static lv_obj_t *s_gif;
static lv_obj_t *s_label;
static lv_img_dsc_t s_gif_dsc;

// --- State ---
static volatile bool s_playing;
static bool s_backlight_off = true;
static void (*s_tap_cb)(void);
static TimerHandle_t s_idle_timer;
static int s_gif_count;

// --- GIF display helpers ---

static void show_gif_data(const uint8_t *data, size_t size)
{
    s_gif_dsc.header.always_zero = 0;
    s_gif_dsc.header.cf = LV_IMG_CF_RAW;
    s_gif_dsc.header.w = 0;
    s_gif_dsc.header.h = 0;
    s_gif_dsc.data_size = size;
    s_gif_dsc.data = data;

    lv_gif_set_src(s_gif, &s_gif_dsc);

    lv_img_dsc_t *dsc = (lv_img_dsc_t *)lv_img_get_src(s_gif);
    if (dsc && dsc->header.w > 0) {
        uint16_t zoom = (uint16_t)((DRV_LCD_H_RES * 256) / dsc->header.w);
        lv_img_set_zoom(s_gif, zoom);
    }
    lv_obj_center(s_gif);
}

static void show_blank(void)
{
    size_t size;
    const uint8_t *data = gif_loader_get_blank(&size);
    if (!data)
        return;
    s_playing = false;
    show_gif_data(data, size);
    xTimerReset(s_idle_timer, 0);
}

static uint16_t get_full_zoom(void)
{
    lv_img_dsc_t *dsc = (lv_img_dsc_t *)lv_img_get_src(s_gif);
    if (dsc && dsc->header.w > 0)
        return (uint16_t)((DRV_LCD_H_RES * 256) / dsc->header.w);
    return 256;
}

// --- LVGL event callbacks ---

static void gif_done_cb(lv_event_t *e)
{
    (void)e;
    show_blank();
}

static void zoom_anim_cb(void *obj, int32_t val)
{
    lv_img_set_zoom((lv_obj_t *)obj, (uint16_t)val);
}

static void screen_press_cb(lv_event_t *e)
{
    (void)e;
    if (s_playing)
        return;

    sound_play_pop();

    uint16_t full = get_full_zoom();
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_gif);
    lv_anim_set_exec_cb(&a, zoom_anim_cb);
    lv_anim_set_values(&a, full, full * 85 / 100);
    lv_anim_set_time(&a, 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);
}

/// Loading on release avoids touch I2C polling competing with SD card SPI DMA.
static void screen_release_cb(lv_event_t *e)
{
    (void)e;
    if (s_playing)
        return;
    if (s_tap_cb)
        s_tap_cb();

    uint16_t full = get_full_zoom();
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_gif);
    lv_anim_set_exec_cb(&a, zoom_anim_cb);
    lv_anim_set_values(&a, full * 85 / 100, full);
    lv_anim_set_time(&a, 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    screen_next_gif();
}

// --- GIF loader callback (called from loader task) ---

static void on_gif_loaded(uint8_t *data, size_t size)
{
    if (!data) {
        ESP_LOGE(TAG, "Failed to load GIF");
        lvgl_port_lock(0);
        show_blank();
        lvgl_port_unlock();
        return;
    }

    lvgl_port_lock(0);
    show_gif_data(data, size);
    ESP_LOGI(TAG, "Playing (%u bytes)", (unsigned)size);
    lvgl_port_unlock();

    if (s_backlight_off) {
        s_backlight_off = false;
        board_set_lcd_brightness(100);
    }
}

// --- Idle timer ---

static void idle_timer_cb(TimerHandle_t t)
{
    (void)t;
    screen_next_gif();
}

// --- Public API ---

void screen_init(void)
{
    s_gif_count = gif_loader_init();

    s_idle_timer = xTimerCreate("idle", pdMS_TO_TICKS(IDLE_AUTO_PLAY_MS),
                                pdFALSE, NULL, idle_timer_cb);

    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, screen_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, screen_release_cb, LV_EVENT_RELEASED, NULL);

    s_gif = lv_gif_create(scr);
    lv_obj_center(s_gif);
    lv_obj_add_event_cb(s_gif, gif_done_cb, LV_EVENT_READY, NULL);

    s_label = lv_label_create(scr);
    lv_label_set_text(s_label, "");
    lv_label_set_long_mode(s_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_label, lv_pct(90));
    lv_obj_set_style_text_color(s_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(s_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_label);

    if (s_gif_count == 0 && !gif_loader_get_blank(NULL)) {
        lv_label_set_text(s_label, "No GIFs found on SD card");
        board_set_lcd_brightness(100);
    }

    lvgl_port_unlock();

    if (s_gif_count > 0)
        screen_next_gif();
}

void screen_set_tap_cb(void (*cb)(void))
{
    s_tap_cb = cb;
}

void screen_show_text(const char *text)
{
    lvgl_port_lock(0);
    lv_label_set_text(s_label, text);
    lvgl_port_unlock();
}

void screen_next_gif(void)
{
    if (s_gif_count == 0 || s_playing)
        return;
    s_playing = true;
    xTimerStop(s_idle_timer, 0);
    gif_loader_request(on_gif_loaded);
}
