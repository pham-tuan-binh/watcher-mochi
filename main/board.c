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

void board_init(void)
{
    ESP_LOGI(TAG, "Initializing board");

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Wake cause: %d (0=reset, 2=ext0, 4=timer)", (int)cause);

    // IO expander first — restores power rails after deep sleep
    bsp_io_expander_init();
    ESP_ERROR_CHECK(bsp_codec_init());

    // Recover touch I2C bus before LVGL tries to talk to the SPD2010
    touch_i2c_bus_recover();
    vTaskDelay(pdMS_TO_TICKS(200));

    lv_disp_t *disp = bsp_lvgl_init();
    assert(disp);

    ESP_LOGI(TAG, "Board init done");
}

// --- Button callbacks ---

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
    if (!enc) { ESP_LOGE(TAG, "No encoder found"); return; }
    lvgl_port_encoder_btn_register_event_cb(enc, BUTTON_PRESS_DOWN, btn_cb_wrapper, cb);
}

void board_set_btn_release_cb(void (*cb)(void))
{
    lv_indev_t *enc = find_encoder();
    if (!enc) { ESP_LOGE(TAG, "No encoder found"); return; }
    lvgl_port_encoder_btn_register_event_cb(enc, BUTTON_PRESS_UP, btn_cb_wrapper, cb);
}

void board_set_btn_long_press_cb(void (*cb)(void))
{
    lv_indev_t *enc = find_encoder();
    if (!enc) { ESP_LOGE(TAG, "No encoder found"); return; }
    lvgl_port_encoder_btn_register_event_cb(enc, BUTTON_LONG_PRESS_UP, btn_cb_wrapper, cb);
}

// --- Deep sleep ---

/// Read PCA9535 input register over I2C to clear the interrupt latch.
static void io_expander_clear_interrupt(void)
{
    uint8_t reg = 0x00;
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

    // Power off peripherals
    uint32_t pin_mask_sleep = BSP_PWR_SDCARD | BSP_PWR_CODEC_PA | BSP_PWR_GROVE
                            | BSP_PWR_BAT_ADC | BSP_PWR_LCD | BSP_PWR_AI_CHIP;
    bsp_exp_io_set_level(pin_mask_sleep, 0);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Make floating pins into outputs to prevent spurious interrupts.
    // Keep only button + charge/VBUS/standby as inputs.
    uint32_t keep_inputs = BSP_PWR_CHRG_DET | BSP_PWR_STDBY_DET
                         | BSP_PWR_VBUS_IN_DET | BSP_KNOB_BTN;
    uint32_t pins_to_output = DRV_IO_EXP_INPUT_MASK & ~keep_inputs;
    esp_io_expander_set_dir(io_exp, pins_to_output, IO_EXPANDER_OUTPUT);
    esp_io_expander_set_level(io_exp, pins_to_output, 0);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Clear pending IO expander interrupt
    for (int i = 0; i < 10; i++) {
        io_expander_clear_interrupt();
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(BSP_IO_EXPANDER_INT) == 1)
            break;
    }

    if (time_sec > 0)
        esp_sleep_enable_timer_wakeup((uint64_t)time_sec * 1000000);

    esp_sleep_enable_ext0_wakeup(BSP_IO_EXPANDER_INT, 0);
    rtc_gpio_pullup_en(BSP_IO_EXPANDER_INT);
    rtc_gpio_pulldown_dis(BSP_IO_EXPANDER_INT);

    ESP_LOGI(TAG, "Entering deep sleep");
    esp_deep_sleep_start();
}

void board_set_lcd_brightness(int percent)
{
    bsp_lcd_brightness_set(percent);
}
