#include <assert.h>

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_io_expander_pca95xx_16bit.h"
#include "sensecap-watcher.h"
#include "board.h"

static const char *TAG = "board";

/**
 * Recover the touch I2C bus. bsp_i2c_bus_init() configures the touch I2C pins
 * (GPIO 38/39) as outputs driven low, which can leave the SPD2010 touch
 * controller in a stuck state. Toggle SCL 9 times followed by a STOP condition
 * to force any stuck slave to release the bus.
 */
static void touch_i2c_bus_recover(void)
{
    gpio_set_direction(BSP_TOUCH_I2C_SDA, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BSP_TOUCH_I2C_SDA, GPIO_PULLUP_ONLY);
    gpio_set_direction(BSP_TOUCH_I2C_SCL, GPIO_MODE_OUTPUT);
    gpio_set_pull_mode(BSP_TOUCH_I2C_SCL, GPIO_PULLUP_ONLY);

    for (int i = 0; i < 9; i++) {
        gpio_set_level(BSP_TOUCH_I2C_SCL, 1);
        esp_rom_delay_us(5);
        gpio_set_level(BSP_TOUCH_I2C_SCL, 0);
        esp_rom_delay_us(5);
    }

    /* STOP condition: SDA transitions low-to-high while SCL is high */
    gpio_set_direction(BSP_TOUCH_I2C_SDA, GPIO_MODE_OUTPUT);
    gpio_set_level(BSP_TOUCH_I2C_SDA, 0);
    esp_rom_delay_us(5);
    gpio_set_level(BSP_TOUCH_I2C_SCL, 1);
    esp_rom_delay_us(5);
    gpio_set_level(BSP_TOUCH_I2C_SDA, 1);
    esp_rom_delay_us(5);
}

void board_init()
{
    ESP_LOGI(TAG, "Initializing board");

    // Log wake reason
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Wake cause: %d (0=reset, 2=ext0, 4=timer)", (int)cause);

    // Initialize IO expander early to restore power rails. After deep sleep
    // the IO expander retains powered-off state — the touch controller needs
    // time to boot after BSP_PWR_LCD is re-enabled.
    ESP_LOGI(TAG, "Init IO expander");
    bsp_io_expander_init();

    // Initialize codec (also sets up I2C bus and audio I2S)
    ESP_LOGI(TAG, "Init codec");
    ESP_ERROR_CHECK(bsp_codec_init());

    // Recover touch I2C bus before LVGL init attempts to talk to the SPD2010
    ESP_LOGI(TAG, "Recovering touch I2C bus");
    touch_i2c_bus_recover();

    // Let the touch controller boot after power was restored
    vTaskDelay(pdMS_TO_TICKS(200));

    // Initialize display and LVGL
    ESP_LOGI(TAG, "Init LVGL");
    lv_disp_t *disp = bsp_lvgl_init();
    assert(disp);

    // BSP_LCD_DEFAULT_BRIGHTNESS=0 keeps backlight off during init.
    // screen_init() turns it on after painting the first frame.
    ESP_LOGI(TAG, "Board init done");
}

/// Find the encoder input device registered by the BSP.
static lv_indev_t *find_encoder(void)
{
    lv_indev_t *indev = NULL;
    while (1) {
        indev = lv_indev_get_next(indev);
        if (indev == NULL || indev->driver->type == LV_INDEV_TYPE_ENCODER)
            break;
    }
    return indev;
}

static void btn_cb_wrapper(void *arg, void *arg2)
{
    void (*cb)(void) = arg2;
    if (cb) cb();
}

void board_set_btn_press_cb(void (*cb)(void))
{
    lv_indev_t *enc = find_encoder();
    if (enc == NULL) { ESP_LOGE(TAG, "No encoder found"); return; }
    lvgl_port_encoder_btn_register_event_cb(enc, BUTTON_PRESS_DOWN, btn_cb_wrapper, cb);
}

void board_set_btn_release_cb(void (*cb)(void))
{
    lv_indev_t *enc = find_encoder();
    if (enc == NULL) { ESP_LOGE(TAG, "No encoder found"); return; }
    lvgl_port_encoder_btn_register_event_cb(enc, BUTTON_PRESS_UP, btn_cb_wrapper, cb);
}

void board_set_btn_long_press_cb(void (*cb)(void))
{
    lv_indev_t *enc = find_encoder();
    if (enc == NULL) { ESP_LOGE(TAG, "No encoder found"); return; }
    lvgl_port_encoder_btn_register_event_cb(enc, BUTTON_LONG_PRESS_UP, btn_cb_wrapper, cb);
}

/// Read the PCA9535 input register over I2C to clear the interrupt latch.
/// The driver caches reads, so bsp_exp_io_get_level() won't actually hit
/// the bus — we must do a raw I2C transaction.
static void io_expander_clear_interrupt(void)
{
    uint8_t reg = 0x00; // PCA9535 Input Port 0 register
    uint8_t buf[2];
    esp_err_t err = i2c_master_write_read_device(BSP_GENERAL_I2C_NUM,
        ESP_IO_EXPANDER_I2C_PCA9535_ADDRESS_001,
        &reg, 1, buf, 2, pdMS_TO_TICKS(100));
    ESP_LOGD(TAG, "IO exp clear INT: err=%d, port0=0x%02x, port1=0x%02x",
             err, buf[0], buf[1]);
}

void board_deep_sleep(uint32_t time_sec)
{
    ESP_LOGI(TAG, "Deep sleep requested (timer=%lus)", (unsigned long)time_sec);

    esp_io_expander_handle_t io_exp = bsp_io_expander_init();

    // Turn off peripherals via IO expander
    uint32_t pin_mask_sleep = BSP_PWR_SDCARD | BSP_PWR_CODEC_PA | BSP_PWR_GROVE
                            | BSP_PWR_BAT_ADC | BSP_PWR_LCD | BSP_PWR_AI_CHIP;
    bsp_exp_io_set_level(pin_mask_sleep, 0);
    ESP_LOGI(TAG, "Peripherals powered off");

    vTaskDelay(pdMS_TO_TICKS(100));

    // Reconfigure IO expander: make pins connected to powered-off devices
    // into outputs so floating signals don't trigger spurious interrupts.
    // Keep only the button (P0.3) and charge/VBUS/standby (P0.0-P0.2) as inputs.
    uint32_t keep_inputs = BSP_PWR_CHRG_DET | BSP_PWR_STDBY_DET
                         | BSP_PWR_VBUS_IN_DET | BSP_KNOB_BTN;
    uint32_t pins_to_output = DRV_IO_EXP_INPUT_MASK & ~keep_inputs;
    esp_io_expander_set_dir(io_exp, pins_to_output, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io_exp, pins_to_output, 0);
    ESP_LOGI(TAG, "Masked floating inputs: 0x%04lx", (unsigned long)pins_to_output);

    vTaskDelay(pdMS_TO_TICKS(50));

    // Clear any pending interrupt by reading the input register over I2C.
    for (int i = 0; i < 10; i++) {
        io_expander_clear_interrupt();
        vTaskDelay(pdMS_TO_TICKS(50));
        int level = gpio_get_level(BSP_IO_EXPANDER_INT);
        ESP_LOGI(TAG, "INT clear attempt %d: GPIO%d=%d", i + 1,
                 BSP_IO_EXPANDER_INT, level);
        if (level == 1)
            break;
    }

    int final_level = gpio_get_level(BSP_IO_EXPANDER_INT);
    ESP_LOGI(TAG, "Final IO_EXP_INT: %d (%s)", final_level,
             final_level == 1 ? "OK" : "WARN — may wake immediately");

    if (time_sec > 0)
        esp_sleep_enable_timer_wakeup((uint64_t)time_sec * 1000000);

    esp_sleep_enable_ext0_wakeup(BSP_IO_EXPANDER_INT, 0);
    rtc_gpio_pullup_en(BSP_IO_EXPANDER_INT);
    rtc_gpio_pulldown_dis(BSP_IO_EXPANDER_INT);

    ESP_LOGI(TAG, "Entering deep sleep now");
    esp_deep_sleep_start();
}

void board_set_lcd_brightness(int percent)
{
    bsp_lcd_brightness_set(percent);
}

// --- Pop sound ---

#include "esp_random.h"
#include "pop_sound.h"

#define POP_VOLUME 60

static int16_t s_pop_buf[POP_SOUND_SAMPLES];
static TaskHandle_t s_pop_task;

static void pop_task(void *arg)
{
    (void)arg;
    bsp_codec_volume_set(POP_VOLUME, NULL);

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Apply random pitch variation (±8%)
        uint32_t rand = esp_random();
        int pitch_shift = 236 + (int)(rand % 41);

        int out_len = 0;
        for (int src = 0; src < POP_SOUND_SAMPLES && out_len < POP_SOUND_SAMPLES; ) {
            s_pop_buf[out_len++] = pop_sound_data[src];
            src = (int)((int64_t)out_len * pitch_shift / 256);
        }

        size_t written;
        bsp_i2s_write(s_pop_buf, out_len * sizeof(int16_t), &written, 500);
    }
}

void board_play_pop(void)
{
    if (!s_pop_task) {
        xTaskCreate(pop_task, "pop", 4096, NULL, 5, &s_pop_task);
    }
    xTaskNotifyGive(s_pop_task);
}
