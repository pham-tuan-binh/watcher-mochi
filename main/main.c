#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "board.h"
#include "screen.h"

static const char *TAG = "main";

#define INACTIVITY_TIMEOUT_MS (CONFIG_MOCHI_DEEP_SLEEP_TIMEOUT_SEC * 1000)

static TimerHandle_t s_inactivity_timer;

static void inactivity_timer_cb(TimerHandle_t t)
{
    (void)t;
    ESP_LOGI(TAG, "Inactivity timeout — entering deep sleep");
    board_deep_sleep(0);
}

static void on_long_press(void)
{
    ESP_LOGI(TAG, "Button long press — entering deep sleep");
    board_deep_sleep(0);
}

static void reset_inactivity_timer(void)
{
    xTimerReset(s_inactivity_timer, 0);
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

    board_init();
    screen_init();

    s_inactivity_timer = xTimerCreate("inact", pdMS_TO_TICKS(INACTIVITY_TIMEOUT_MS),
                                       pdFALSE, NULL, inactivity_timer_cb);
    xTimerStart(s_inactivity_timer, 0);

    board_set_btn_press_cb(reset_inactivity_timer);
    board_set_btn_long_press_cb(on_long_press);
    screen_set_tap_cb(reset_inactivity_timer);
}
