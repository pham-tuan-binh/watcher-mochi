
/*
 * SPDX-FileCopyrightText: 2024 Seeed Tech. Co., Ltd.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <time.h>
#include <sys/time.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/i2c.h"
#include "driver/uart.h"
#include "driver/spi_master.h"
#include "driver/i2s_std.h"
#include "driver/rtc_io.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "iot_button.h"
#include "iot_knob.h"
#include "led_strip.h"
#include "led_strip_interface.h"
#include "esp_io_expander.h"
#include "esp_io_expander_pca95xx_16bit.h"
#include "esp_lcd_spd2010.h"
#include "esp_lcd_touch_spd2010.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "esp_sleep.h"
#include "sdmmc_cmd.h"
#include "sscma_client.h"

/* SPI */
#define BSP_SPI2_HOST_SCLK (GPIO_NUM_4)
#define BSP_SPI2_HOST_MOSI (GPIO_NUM_5)
#define BSP_SPI2_HOST_MISO (GPIO_NUM_6)

/* QSPI */
#define BSP_SPI3_HOST_PCLK  (GPIO_NUM_7)
#define BSP_SPI3_HOST_DATA0 (GPIO_NUM_9)
#define BSP_SPI3_HOST_DATA1 (GPIO_NUM_1)
#define BSP_SPI3_HOST_DATA2 (GPIO_NUM_14)
#define BSP_SPI3_HOST_DATA3 (GPIO_NUM_13)

/* RGB LED */
#define BSP_RGB_CTRL (GPIO_NUM_40)

/* ADC */
#define BSP_BAT_ADC_CHAN  (ADC_CHANNEL_2)    // GPIO3
#define BSP_BAT_ADC_ATTEN (ADC_ATTEN_DB_2_5) // 0 ~ 1100 mV
#define BSP_BAT_VOL_RATIO ((62 + 20) / 20)

/* Knob */
#define BSP_KNOB_A   (GPIO_NUM_41)
#define BSP_KNOB_B   (GPIO_NUM_42)
#define BSP_KNOB_BTN (IO_EXPANDER_PIN_NUM_3)

/* Himax */
#define BSP_SSCMA_CLIENT_RST              (IO_EXPANDER_PIN_NUM_7)
#define BSP_SSCMA_CLIENT_RST_USE_EXPANDER (true)

#define BSP_SSCMA_CLIENT_SPI_NUM               (SPI2_HOST)
#define BSP_SSCMA_CLIENT_SPI_CS                (GPIO_NUM_21)
#define BSP_SSCMA_CLIENT_SPI_SYNC              (IO_EXPANDER_PIN_NUM_6)
#define BSP_SSCMA_CLIENT_SPI_SYNC_USE_EXPANDER (true)
#define BSP_SSCMA_CLIENT_SPI_CLK               (12 * 1000 * 1000)

#define BSP_SSCMA_FLASHER_UART_NUM       (UART_NUM_1)
#define BSP_SSCMA_FLASHER_UART_TX        (GPIO_NUM_17)
#define BSP_SSCMA_FLASHER_UART_RX        (GPIO_NUM_18)
#define BSP_SSCMA_FLASHER_UART_BAUD_RATE (921600)

/* SD Card */
#define BSP_SD_SPI_NUM  (SPI2_HOST)
#define BSP_SD_SPI_CS   (GPIO_NUM_46)
#define BSP_SD_GPIO_DET (IO_EXPANDER_PIN_NUM_4)

/* LCD */
#define BSP_LCD_SPI_NUM  (SPI3_HOST)
#define BSP_LCD_SPI_CS   (GPIO_NUM_45)
#define BSP_LCD_GPIO_RST (GPIO_NUM_NC)
#define BSP_LCD_GPIO_DC  (GPIO_NUM_1)
#define BSP_LCD_GPIO_BL  (GPIO_NUM_8)

/* Touch */
#define BSP_TOUCH_I2C_NUM  (1)
#define BSP_TOUCH_GPIO_INT (IO_EXPANDER_PIN_NUM_5)
#define BSP_TOUCH_I2C_SDA  (GPIO_NUM_39)
#define BSP_TOUCH_I2C_SCL  (GPIO_NUM_38)
#define BSP_TOUCH_I2C_CLK  (400000)

/* General */
#define BSP_GENERAL_I2C_NUM (0)
#define BSP_GENERAL_I2C_SDA (GPIO_NUM_47)
#define BSP_GENERAL_I2C_SCL (GPIO_NUM_48)
#define BSP_IO_EXPANDER_INT (GPIO_NUM_2)
#define BSP_GENERAL_I2C_CLK (400000)

/* Audio */
#define BSP_AUDIO_I2S_NUM  (0)
#define BSP_AUDIO_I2S_MCLK (GPIO_NUM_10)
#define BSP_AUDIO_I2S_SCLK (GPIO_NUM_11)
#define BSP_AUDIO_I2S_LRCK (GPIO_NUM_12)
#define BSP_AUDIO_I2S_DSIN (GPIO_NUM_15)
#define BSP_AUDIO_I2S_DOUT (GPIO_NUM_16)

/* POWER */
#define BSP_PWR_CHRG_DET    (IO_EXPANDER_PIN_NUM_0)
#define BSP_PWR_STDBY_DET   (IO_EXPANDER_PIN_NUM_1)
#define BSP_PWR_VBUS_IN_DET (IO_EXPANDER_PIN_NUM_2)
#define BSP_PWR_SDCARD      (IO_EXPANDER_PIN_NUM_8)
#define BSP_PWR_LCD         (IO_EXPANDER_PIN_NUM_9)
#define BSP_PWR_SYSTEM      (IO_EXPANDER_PIN_NUM_10)
#define BSP_PWR_AI_CHIP     (IO_EXPANDER_PIN_NUM_11)
#define BSP_PWR_CODEC_PA    (IO_EXPANDER_PIN_NUM_12)
#define BSP_PWR_BAT_DET     (IO_EXPANDER_PIN_NUM_13)
#define BSP_PWR_GROVE       (IO_EXPANDER_PIN_NUM_14)
#define BSP_PWR_BAT_ADC     (IO_EXPANDER_PIN_NUM_15)

/* Settings */
#define DRV_LCD_H_RES             (412)
#define DRV_LCD_V_RES             (412)
#define DRV_LCD_PIXEL_CLK_HZ      (40 * 1000 * 1000)
#define DRV_LCD_CMD_BITS          (32)
#define DRV_LCD_PARAM_BITS        (8)
#define DRV_LCD_RGB_ELEMENT_ORDER (LCD_RGB_ELEMENT_ORDER_RGB)
#define DRV_LCD_BITS_PER_PIXEL    (16)
#define DRV_LCD_SWAP_XY           (0)
#define DRV_LCD_MIRROR_X          (0)
#define DRV_LCD_MIRROR_Y          (0)

#define DRV_LCD_BL_ON_LEVEL   (1)
#define DRV_LCD_LEDC_DUTY_RES (LEDC_TIMER_10_BIT)
#define DRV_LCD_LEDC_CH       (1)

#define DRV_IO_EXP_INPUT_MASK  (0x20ff) // P0.0 ~ P0.7 | P1.3
#define DRV_IO_EXP_OUTPUT_MASK (0xDf00) // P1.0 ~ P1.7 & ~P1.3

#define DRV_PCF8563_I2C_ADDR   (0x51)
#define DRV_PCF8563_TIMEOUT_MS (1000)
#define DRV_RTC_REG_STATUS1    (0x00)
#define DRV_RTC_REG_STATUS2    (0x01)
#define DRV_RTC_REG_TIME       (0x02)
#define DRV_RTC_REG_ALARM      (0x09)
#define DRV_RTC_REG_CLKOUT     (0x0d)
#define DRV_RTC_REG_TIMER_CTL  (0x0e)
#define DRV_RTC_REG_TIMER      (0x0f)

#define DRV_ES8311_I2C_ADDR   (0x30)
#define DRV_ES7243_I2C_ADDR   (0x13)
#define DRV_ES7243E_I2C_ADDR  (0x14)
#define DRV_AUDIO_SAMPLE_RATE (16000)
#define DRV_AUDIO_SAMPLE_BITS (16)
#define DRV_AUDIO_CHANNELS    (1)
#define DRV_AUDIO_MIC_GAIN    (27.0)
#define DRV_AUDIO_I2S_CHANNEL (1)

#define LVGL_DRAW_BUFF_DOUBLE (1)
#define LVGL_DRAW_BUFF_HEIGHT (CONFIG_LVGL_DRAW_BUFF_HEIGHT)

#define DRV_FS_MAX_FILES    (10)
#define DRV_BASE_PATH_SD    "/sdcard"
#define DRV_BASE_PATH_FLASH "/spiffs"

#define BSP_PWR_START_UP (BSP_PWR_SDCARD | BSP_PWR_LCD | BSP_PWR_SYSTEM | BSP_PWR_AI_CHIP | BSP_PWR_CODEC_PA | BSP_PWR_GROVE | BSP_PWR_BAT_ADC)

#define DEC2BCD(d) (((((d) / 10) & 0x0f) << 4) + (((d) % 10) & 0x0f))
#define BCD2DEC(b) (((((b) >> 4) & 0x0F) * 10) + ((b) & 0x0F))

#define BSP_I2S_GPIO_CFG                                                                                                                                                                               \
    {                                                                                                                                                                                                  \
        .mclk = BSP_AUDIO_I2S_MCLK, .bclk = BSP_AUDIO_I2S_SCLK, .ws = BSP_AUDIO_I2S_LRCK, .dout = BSP_AUDIO_I2S_DOUT, .din = BSP_AUDIO_I2S_DSIN,                                                       \
        .invert_flags = {                                                                                                                                                                              \
            .mclk_inv = false,                                                                                                                                                                         \
            .bclk_inv = false,                                                                                                                                                                         \
            .ws_inv = false,                                                                                                                                                                           \
        },                                                                                                                                                                                             \
    }

#define BSP_I2S_SLOT_CONFIG(bits_per_sample, mono_or_stereo)                                                                                                                                           \
    {                                                                                                                                                                                                  \
        .data_bit_width = bits_per_sample, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO, .slot_mode = mono_or_stereo, .slot_mask = I2S_STD_SLOT_BOTH, .ws_width = bits_per_sample, .ws_pol = false,       \
        .bit_shift = true, .left_align = true, .big_endian = false, .bit_order_lsb = false                                                                                                             \
    }

#define BSP_I2S_DUPLEX_MONO_CFG(_sample_rate)                                                                                                                                                          \
    {                                                                                                                                                                                                  \
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate), .slot_cfg = BSP_I2S_SLOT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO), .gpio_cfg = BSP_I2S_GPIO_CFG,                              \
    }

#ifdef __cplusplus
extern "C" {
#endif

#if CONFIG_BSP_ERROR_CHECK
#define BSP_ERROR_CHECK_RETURN_ERR(x)    ESP_ERROR_CHECK(x)
#define BSP_ERROR_CHECK_RETURN_NULL(x)   ESP_ERROR_CHECK(x)
#define BSP_ERROR_CHECK(x, ret)          ESP_ERROR_CHECK(x)
#define BSP_NULL_CHECK(x, ret)           assert(x)
#define BSP_NULL_CHECK_GOTO(x, goto_tag) assert(x)
#else
#define BSP_ERROR_CHECK_RETURN_ERR(x)                                                                                                                                                                  \
    do                                                                                                                                                                                                 \
    {                                                                                                                                                                                                  \
        esp_err_t err_rc_ = (x);                                                                                                                                                                       \
        if (unlikely(err_rc_ != ESP_OK))                                                                                                                                                               \
        {                                                                                                                                                                                              \
            return err_rc_;                                                                                                                                                                            \
        }                                                                                                                                                                                              \
    }                                                                                                                                                                                                  \
    while (0)

#define BSP_ERROR_CHECK_RETURN_NULL(x)                                                                                                                                                                 \
    do                                                                                                                                                                                                 \
    {                                                                                                                                                                                                  \
        if (unlikely((x) != ESP_OK))                                                                                                                                                                   \
        {                                                                                                                                                                                              \
            return NULL;                                                                                                                                                                               \
        }                                                                                                                                                                                              \
    }                                                                                                                                                                                                  \
    while (0)

#define BSP_NULL_CHECK(x, ret)                                                                                                                                                                         \
    do                                                                                                                                                                                                 \
    {                                                                                                                                                                                                  \
        if ((x) == NULL)                                                                                                                                                                               \
        {                                                                                                                                                                                              \
            return ret;                                                                                                                                                                                \
        }                                                                                                                                                                                              \
    }                                                                                                                                                                                                  \
    while (0)

#define BSP_ERROR_CHECK(x, ret)                                                                                                                                                                        \
    do                                                                                                                                                                                                 \
    {                                                                                                                                                                                                  \
        if (unlikely((x) != ESP_OK))                                                                                                                                                                   \
        {                                                                                                                                                                                              \
            return ret;                                                                                                                                                                                \
        }                                                                                                                                                                                              \
    }                                                                                                                                                                                                  \
    while (0)

#define BSP_NULL_CHECK_GOTO(x, goto_tag)                                                                                                                                                               \
    do                                                                                                                                                                                                 \
    {                                                                                                                                                                                                  \
        if ((x) == NULL)                                                                                                                                                                               \
        {                                                                                                                                                                                              \
            goto goto_tag;                                                                                                                                                                             \
        }                                                                                                                                                                                              \
    }                                                                                                                                                                                                  \
    while (0)
#endif

typedef struct
{
    lvgl_port_cfg_t lvgl_port_cfg; /*!< LVGL port configuration */
    uint32_t buffer_size;          /*!< Size of the buffer for the screen in pixels */
    bool double_buffer;            /*!< True, if should be allocated two buffers */
    struct
    {
        unsigned int buff_dma : 1;    /*!< Allocated LVGL buffer will be DMA capable */
        unsigned int buff_spiram : 1; /*!< Allocated LVGL buffer will be in PSRAM */
    } flags;
} bsp_display_cfg_t;

esp_err_t bsp_i2c_bus_init(void);
esp_err_t bsp_spi_bus_init(void);
esp_err_t bsp_uart_bus_init(void);

esp_err_t bsp_rgb_init(void);
esp_err_t bsp_rgb_set(uint8_t r, uint8_t g, uint8_t b);

uint16_t bsp_battery_get_voltage(void);
uint8_t bsp_battery_get_percent(void);

esp_err_t bsp_i2c_detect(i2c_port_t i2c_num);

void bsp_system_deep_sleep(uint32_t time_in_sec);
void bsp_system_reboot(void);
void bsp_system_shutdown(void);
bool bsp_system_is_charging(void);
bool bsp_system_is_standby(void);
bool bsp_battery_is_present(void);

esp_err_t bsp_rtc_init(void);
esp_err_t bsp_rtc_get_time(struct tm *timeinfo);
esp_err_t bsp_rtc_set_time(const struct tm *timeinfo);
esp_err_t bsp_rtc_set_timer(uint32_t time_in_sec);

esp_err_t bsp_knob_btn_init(void *param);
uint8_t bsp_knob_btn_get_key_value(void *param);
esp_err_t bsp_knob_btn_deinit(void *param);
void bsp_set_btn_long_press_cb(void (*cb)(void));
void bsp_set_btn_long_release_cb(void (*cb)(void));

esp_err_t bsp_lcd_brightness_set(int brightness_percent);
esp_lcd_panel_handle_t bsp_lcd_get_panel_handle(void);
esp_lcd_touch_handle_t bsp_lcd_get_touch_handle(void);

lv_disp_t *bsp_lvgl_init(void);
lv_disp_t *bsp_lvgl_init_with_cfg(const bsp_display_cfg_t *cfg);
lv_disp_t *bsp_lvgl_get_disp(void);

sscma_client_handle_t bsp_sscma_client_init();
sscma_client_flasher_handle_t bsp_sscma_flasher_init();

esp_io_expander_handle_t bsp_io_expander_init();
uint8_t bsp_exp_io_get_level(uint16_t pin_mask);
esp_err_t bsp_exp_io_set_level(uint16_t pin_mask, uint8_t level);

bool bsp_sdcard_is_inserted(void);
esp_err_t bsp_sdcard_init(char *mount_point, size_t max_files);
esp_err_t bsp_sdcard_init_default(void);
esp_err_t bsp_sdcard_deinit(char *mount_point);
esp_err_t bsp_sdcard_deinit_default(void);

esp_err_t bsp_spiffs_init(char *mount_point, size_t max_files);
esp_err_t bsp_spiffs_init_default(void);

esp_err_t bsp_audio_init(const i2s_std_config_t *i2s_config);
const audio_codec_data_if_t *bsp_audio_get_codec_itf(void);
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);
esp_codec_dev_handle_t bsp_audio_codec_microphone_init(void);
esp_err_t bsp_i2s_read(void *audio_buffer, size_t len, size_t *bytes_read, uint32_t timeout_ms);
esp_err_t bsp_i2s_write(void *audio_buffer, size_t len, size_t *bytes_written, uint32_t timeout_ms);
esp_err_t bsp_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch);
esp_err_t bsp_codec_volume_set(int volume, int *volume_set);
esp_err_t bsp_codec_mute_set(bool enable);
esp_err_t bsp_codec_dev_stop(void);
esp_err_t bsp_codec_dev_resume(void);

esp_err_t bsp_codec_init(void);
esp_codec_dev_handle_t bsp_codec_speaker_get(void);
esp_codec_dev_handle_t bsp_codec_microphone_get(void);
esp_err_t bsp_get_feed_data(bool is_get_raw_channel, int16_t *buffer, int buffer_len);
int bsp_get_feed_channel(void);

#ifdef __cplusplus
}
#endif