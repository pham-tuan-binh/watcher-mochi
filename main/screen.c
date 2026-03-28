#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sensecap-watcher.h"
#include "board.h"
#include "screen.h"

static const char *TAG = "screen";

#define SD_MOUNT_POINT "/sdcard"
#define GIF_DIR        SD_MOUNT_POINT
#define MAX_GIFS       100
#define READ_CHUNK_SIZE  (64 * 1024)

static lv_obj_t *s_label;
static lv_obj_t *s_gif;

static char *s_gif_files[MAX_GIFS];
static int s_gif_count;

static uint8_t *s_gif_data;
static lv_img_dsc_t s_gif_dsc;

/// Cached blank.gif data (always in memory).
static uint8_t *s_blank_data;
static size_t s_blank_size;

/// DMA-capable bounce buffer reused across reads.
static uint8_t *s_dma_buf;

/// Auto-play timer: triggers a random GIF after blank shows for too long.
#define BLANK_IDLE_MS 30000
static TimerHandle_t s_idle_timer;

/// Background task that handles blocking SD card reads.
static TaskHandle_t s_loader_task;

/// True while a non-blank GIF is playing — blocks new triggers.
static volatile bool s_playing;

/// Backlight not yet on — first GIF render turns it on.
static bool s_backlight_off = true;

/// Optional callback invoked on screen tap (for inactivity timer reset).
static void (*s_tap_cb)(void);

static void scan_gif_files(void)
{
    DIR *dir = opendir(GIF_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory %s", GIF_DIR);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_gif_count < MAX_GIFS) {
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcasecmp(entry->d_name + len - 4, ".gif") == 0
                && strncmp(entry->d_name, "._", 2) != 0
                && strcasecmp(entry->d_name, "blank.gif") != 0) {
            char path[256];
            snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", entry->d_name);
            s_gif_files[s_gif_count] = strdup(path);
            if (s_gif_files[s_gif_count]) {
                ESP_LOGI(TAG, "Found GIF: %s", s_gif_files[s_gif_count]);
                s_gif_count++;
            }
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "Found %d GIF files", s_gif_count);
}

/// Read file into SPIRAM via DMA bounce buffer. Returns size or 0 on failure.
static size_t read_file(const char *path, uint8_t **out)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return 0;

    uint8_t *buf = malloc(st.st_size);
    if (!buf)
        return 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        free(buf);
        return 0;
    }

    uint32_t t0 = xTaskGetTickCount();

    size_t total = 0;
    while (total < (size_t)st.st_size) {
        size_t to_read = (size_t)st.st_size - total;
        if (to_read > READ_CHUNK_SIZE)
            to_read = READ_CHUNK_SIZE;
        ssize_t n = read(fd, s_dma_buf, to_read);
        if (n <= 0)
            break;
        memcpy(buf + total, s_dma_buf, n);
        total += n;
    }
    close(fd);

    uint32_t elapsed = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "Read %u bytes in %lu ms (%.1f KB/s)",
             (unsigned)total, (unsigned long)elapsed,
             elapsed > 0 ? (total / 1024.0f) / (elapsed / 1000.0f) : 0);

    if (total != (size_t)st.st_size) {
        free(buf);
        return 0;
    }

    *out = buf;
    return total;
}

/// Display a GIF from an in-memory buffer. Caller must hold the LVGL lock.
static void show_gif_data_locked(const uint8_t *data, size_t size)
{
    s_gif_dsc.header.always_zero = 0;
    s_gif_dsc.header.cf = LV_IMG_CF_RAW;
    s_gif_dsc.header.w = 0;
    s_gif_dsc.header.h = 0;
    s_gif_dsc.data_size = size;
    s_gif_dsc.data = data;

    lv_gif_set_src(s_gif, &s_gif_dsc);

    // Scale to screen width
    lv_img_dsc_t *dsc = (lv_img_dsc_t *)lv_img_get_src(s_gif);
    if (dsc && dsc->header.w > 0) {
        uint16_t zoom = (uint16_t)((DRV_LCD_H_RES * 256) / dsc->header.w);
        lv_img_set_zoom(s_gif, zoom);
    }
    lv_obj_center(s_gif);
}

/// Show the idle blank GIF. Caller must hold the LVGL lock.
static void show_blank_locked(void)
{
    if (!s_blank_data)
        return;
    s_playing = false;
    show_gif_data_locked(s_blank_data, s_blank_size);
    ESP_LOGI(TAG, "Showing blank");

    // Start idle countdown — auto-play a random GIF after 5s
    xTimerReset(s_idle_timer, 0);
}

/// Called when a non-looping GIF finishes — return to blank.
static void gif_done_cb(lv_event_t *e)
{
    (void)e;
    show_blank_locked();
}

/// Get the current full-screen zoom value for the GIF widget.
static uint16_t get_full_zoom(void)
{
    lv_img_dsc_t *dsc = (lv_img_dsc_t *)lv_img_get_src(s_gif);
    if (dsc && dsc->header.w > 0)
        return (uint16_t)((DRV_LCD_H_RES * 256) / dsc->header.w);
    return 256;
}

/// Animation callback — sets zoom on the GIF widget.
static void zoom_anim_cb(void *obj, int32_t val)
{
    lv_img_set_zoom((lv_obj_t *)obj, (uint16_t)val);
}

/// Screen press — zoom in + pop sound (visual/audio feedback).
static void screen_press_cb(lv_event_t *e)
{
    (void)e;
    if (s_playing)
        return;

    board_play_pop();

    uint16_t full = get_full_zoom();
    uint16_t small = full * 85 / 100;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_gif);
    lv_anim_set_exec_cb(&a, zoom_anim_cb);
    lv_anim_set_values(&a, full, small);
    lv_anim_set_time(&a, 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);
}

/// Screen release — zoom back out + start loading GIF.
/// Loading on release avoids touch I2C polling competing with SD card SPI DMA.
static void screen_release_cb(lv_event_t *e)
{
    (void)e;
    if (s_playing)
        return;
    if (s_tap_cb) s_tap_cb();

    uint16_t full = get_full_zoom();
    uint16_t small = full * 85 / 100;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_gif);
    lv_anim_set_exec_cb(&a, zoom_anim_cb);
    lv_anim_set_values(&a, small, full);
    lv_anim_set_time(&a, 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    screen_next_gif();
}

/// Background task: waits for a notification, then loads and displays a random GIF.
static void loader_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (s_gif_count == 0)
            continue;

        xTimerStop(s_idle_timer, 0);

        int idx = esp_random() % s_gif_count;
        ESP_LOGI(TAG, "Loading: %s", s_gif_files[idx]);

        uint8_t *data = NULL;
        size_t size = read_file(s_gif_files[idx], &data);
        if (!data) {
            // SD card SPI bus is likely stuck after a CRC error.
            // Remount to reinitialize the bus and card protocol.
            ESP_LOGW(TAG, "Read failed, remounting SD card");
            bsp_sdcard_deinit_default();
            vTaskDelay(pdMS_TO_TICKS(200));
            if (bsp_sdcard_init_default() == ESP_OK) {
                size = read_file(s_gif_files[idx], &data);
            }
        }
        if (!data) {
            ESP_LOGE(TAG, "Failed to read file after remount");
            lvgl_port_lock(0);
            show_blank_locked();
            lvgl_port_unlock();
            continue;
        }

        lvgl_port_lock(0);
        free(s_gif_data);
        s_gif_data = data;
        show_gif_data_locked(s_gif_data, size);
        ESP_LOGI(TAG, "Playing (%u bytes)", (unsigned)size);
        lvgl_port_unlock();

        if (s_backlight_off) {
            s_backlight_off = false;
            board_set_lcd_brightness(100);
        }
    }
}

/// FreeRTOS timer callback — triggers a random GIF from the timer daemon task.
static void idle_timer_cb(TimerHandle_t t)
{
    (void)t;
    screen_next_gif();
}

void screen_init(void)
{
    esp_err_t err = bsp_sdcard_init_default();
    if (err == ESP_OK) {
        scan_gif_files();
    } else {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(err));
    }

    // Allocate reusable DMA bounce buffer
    s_dma_buf = heap_caps_malloc(READ_CHUNK_SIZE, MALLOC_CAP_DMA);

    // Create idle auto-play timer (one-shot)
    s_idle_timer = xTimerCreate("idle", pdMS_TO_TICKS(BLANK_IDLE_MS),
                                pdFALSE, NULL, idle_timer_cb);

    // Background task for non-blocking GIF loading
    xTaskCreate(loader_task, "gif_loader", 4096, NULL, 5, &s_loader_task);

    // Preload blank.gif — stays in memory permanently
    s_blank_size = read_file(SD_MOUNT_POINT "/blank.gif", &s_blank_data);
    if (!s_blank_data) {
        ESP_LOGW(TAG, "blank.gif not found on SD card");
    }

    lvgl_port_lock(0);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    // Make screen tappable
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

    if (s_gif_count == 0 && !s_blank_data) {
        lv_label_set_text(s_label, "No GIFs found on SD card");
        board_set_lcd_brightness(100);
    }

    lvgl_port_unlock();

    // Backlight stays off — loader task turns it on after the first GIF is rendered
    if (s_gif_count > 0) {
        screen_next_gif();
    }
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
    // Signal the loader task — returns immediately, doesn't block the caller
    xTaskNotifyGive(s_loader_task);
}
