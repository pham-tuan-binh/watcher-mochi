/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SPDX-FileContributor: 2024 Seeed Tech. Co., Ltd.
 */

#include "esp_io_expander_pca95xx_16bit.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c.h"

#include "esp_bit_defs.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_log.h"

#include "esp_io_expander.h"

/* Timeout of each I2C communication */
#define I2C_TIMEOUT_MS (30)
/* Re-try times when I2C communication failed */
#define I2C_TRY_NUM (3)

#define IO_COUNT (16)

#define MAX_UPDATE_INTERVAL_US (1000000) /* 1s */

/* Register address */
#define INPUT_REG_ADDR     (0x00)
#define OUTPUT_REG_ADDR    (0x02)
#define DIRECTION_REG_ADDR (0x06)

/* Default register value on power-up */
#define DIR_REG_DEFAULT_VAL (0xffff)
#define OUT_REG_DEFAULT_VAL (0x0000)

/**
 * @brief Device Structure Type
 *
 */
typedef struct
{
    esp_io_expander_t base;
    i2c_port_t i2c_num;
    uint32_t i2c_address;
    gpio_num_t int_gpio;
    volatile bool need_update;
    volatile int64_t last_update_time;
    uint32_t update_interval_us;
    void (*isr_cb)(void *arg);
    void *user_ctx;
    struct
    {
        uint16_t direction;
        uint16_t output;
        uint16_t input;
    } regs;
} esp_io_expander_pca95xx_16bit_t;

static char *TAG = "pca95xx_16";

static esp_err_t read_input_reg(esp_io_expander_handle_t handle, uint32_t *value);
static esp_err_t write_output_reg(esp_io_expander_handle_t handle, uint32_t value);
static esp_err_t read_output_reg(esp_io_expander_handle_t handle, uint32_t *value);
static esp_err_t write_direction_reg(esp_io_expander_handle_t handle, uint32_t value);
static esp_err_t read_direction_reg(esp_io_expander_handle_t handle, uint32_t *value);
static esp_err_t reset(esp_io_expander_t *handle);
static esp_err_t del(esp_io_expander_t *handle);

static void io_exp_isr_handler(void *arg)
{
    esp_io_expander_pca95xx_16bit_t *pca = (esp_io_expander_pca95xx_16bit_t *)arg;
    pca->need_update = true;
    if (pca->isr_cb)
    {
        pca->isr_cb(pca->user_ctx);
    }
}

esp_err_t esp_io_expander_new_i2c_pca95xx_16bit(i2c_port_t i2c_num, uint32_t i2c_address, esp_io_expander_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(i2c_num < I2C_NUM_MAX, ESP_ERR_INVALID_ARG, TAG, "Invalid i2c num");
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    esp_io_expander_pca95xx_16bit_t *pca = (esp_io_expander_pca95xx_16bit_t *)calloc(1, sizeof(esp_io_expander_pca95xx_16bit_t));
    ESP_RETURN_ON_FALSE(pca, ESP_ERR_NO_MEM, TAG, "Malloc failed");

    pca->isr_cb = NULL;
    pca->user_ctx = NULL;
    pca->int_gpio = -1;
    pca->need_update = true;

    pca->base.config.io_count = IO_COUNT;
    pca->base.config.flags.dir_out_bit_zero = 1;
    pca->i2c_num = i2c_num;
    pca->i2c_address = i2c_address;
    pca->base.read_input_reg = read_input_reg;
    pca->base.write_output_reg = write_output_reg;
    pca->base.read_output_reg = read_output_reg;
    pca->base.write_direction_reg = write_direction_reg;
    pca->base.read_direction_reg = read_direction_reg;
    pca->base.del = del;
    pca->base.reset = reset;

    esp_err_t ret = ESP_OK;
    /* Reset configuration and register status */
    ESP_GOTO_ON_ERROR(reset(&pca->base), err, TAG, "Reset failed");

    *handle = &pca->base;
    return ESP_OK;
err:
    free(pca);
    return ret;
}

esp_err_t esp_io_expander_new_i2c_pca95xx_16bit_ex(i2c_port_t i2c_num, uint32_t i2c_address, const pca95xx_16bit_ex_config_t *config, esp_io_expander_handle_t *handle)
{
    ESP_RETURN_ON_FALSE(i2c_num < I2C_NUM_MAX, ESP_ERR_INVALID_ARG, TAG, "Invalid i2c num");
    ESP_RETURN_ON_FALSE(handle, ESP_ERR_INVALID_ARG, TAG, "Invalid handle");

    esp_io_expander_pca95xx_16bit_t *pca = (esp_io_expander_pca95xx_16bit_t *)calloc(1, sizeof(esp_io_expander_pca95xx_16bit_t));
    ESP_RETURN_ON_FALSE(pca, ESP_ERR_NO_MEM, TAG, "Malloc failed");

    pca->int_gpio = config->int_gpio;
    pca->isr_cb = config->isr_cb;
    pca->user_ctx = config->user_ctx;
    pca->need_update = true;

    if (pca->int_gpio != -1)
    {
        const gpio_config_t io_conf = {
            .pin_bit_mask = (1ULL << pca->int_gpio),
            .intr_type = GPIO_INTR_NEGEDGE,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = 1,
        };
        gpio_config(&io_conf);
        gpio_set_intr_type(pca->int_gpio, GPIO_INTR_NEGEDGE);
        gpio_install_isr_service(ESP_INTR_FLAG_SHARED);
        gpio_isr_handler_add(pca->int_gpio, io_exp_isr_handler, pca);
    }

    pca->base.config.io_count = IO_COUNT;
    pca->base.config.flags.dir_out_bit_zero = 1;
    pca->i2c_num = i2c_num;
    pca->i2c_address = i2c_address;
    pca->update_interval_us = config->update_interval_us;
    pca->base.read_input_reg = read_input_reg;
    pca->base.write_output_reg = write_output_reg;
    pca->base.read_output_reg = read_output_reg;
    pca->base.write_direction_reg = write_direction_reg;
    pca->base.read_direction_reg = read_direction_reg;
    pca->base.del = del;
    pca->base.reset = reset;

    esp_err_t ret = ESP_OK;
    /* Reset configuration and register status */
    ESP_GOTO_ON_ERROR(reset(&pca->base), err, TAG, "Reset failed");

    *handle = &pca->base;
    return ESP_OK;
err:
    free(pca);
    return ret;
}

static esp_err_t read_input_reg(esp_io_expander_handle_t handle, uint32_t *value)
{
    esp_io_expander_pca95xx_16bit_t *pca = (esp_io_expander_pca95xx_16bit_t *)__containerof(handle, esp_io_expander_pca95xx_16bit_t, base);

    uint8_t temp[2] = { 0, 0 };
    // *INDENT-OFF*
    if (pca->int_gpio == -1 || pca->need_update || esp_timer_get_time() > (pca->last_update_time + pca->update_interval_us))
    {
        for (uint8_t i = 0; i < I2C_TRY_NUM; i++)
        {
            if (i2c_master_write_read_device(pca->i2c_num, pca->i2c_address, (uint8_t[]) { INPUT_REG_ADDR }, 1, (uint8_t *)&temp, 2, pdMS_TO_TICKS(I2C_TIMEOUT_MS)) == ESP_OK)
            {
                break;
            }
            ESP_LOGW(TAG, "Read input reg failed, retry %d/%d", i + 1, I2C_TRY_NUM);
            ESP_RETURN_ON_FALSE(i < I2C_TRY_NUM - 1, ESP_ERR_INVALID_STATE, TAG, "Read input reg failed"); 
        }
        pca->regs.input = (((uint32_t)temp[1]) << 8) | (temp[0]);
        pca->last_update_time = esp_timer_get_time();
        pca->need_update = false;
    }
    else
    {
        *value = pca->regs.input;
    }
    // *INDENT-ON*
    *value = pca->regs.input & 0xffff;
    return ESP_OK;
}

static esp_err_t write_output_reg(esp_io_expander_handle_t handle, uint32_t value)
{
    esp_io_expander_pca95xx_16bit_t *pca = (esp_io_expander_pca95xx_16bit_t *)__containerof(handle, esp_io_expander_pca95xx_16bit_t, base);
    value &= 0xffff;
    uint8_t data[] = { OUTPUT_REG_ADDR, value & 0xff, value >> 8 };
    for (uint8_t i = 0; i < I2C_TRY_NUM; i++)
    {
        if (i2c_master_write_to_device(pca->i2c_num, pca->i2c_address, data, sizeof(data), pdMS_TO_TICKS(I2C_TIMEOUT_MS)) == ESP_OK)
        {
            break;
        }
        ESP_LOGW(TAG, "Write output reg failed, retry %d/%d", i + 1, I2C_TRY_NUM);
        ESP_RETURN_ON_FALSE(i < I2C_TRY_NUM - 1, ESP_ERR_INVALID_STATE, TAG, "Write output reg failed");
    }
    pca->regs.output = value;
    return ESP_OK;
}

static esp_err_t read_output_reg(esp_io_expander_handle_t handle, uint32_t *value)
{
    esp_io_expander_pca95xx_16bit_t *pca = (esp_io_expander_pca95xx_16bit_t *)__containerof(handle, esp_io_expander_pca95xx_16bit_t, base);

    *value = pca->regs.output;
    return ESP_OK;
}

static esp_err_t write_direction_reg(esp_io_expander_handle_t handle, uint32_t value)
{
    esp_io_expander_pca95xx_16bit_t *pca = (esp_io_expander_pca95xx_16bit_t *)__containerof(handle, esp_io_expander_pca95xx_16bit_t, base);
    value &= 0xffff;

    uint8_t data[] = { DIRECTION_REG_ADDR, value & 0xff, value >> 8 };
    for (uint8_t i = 0; i < I2C_TRY_NUM; i++)
    {
        if (i2c_master_write_to_device(pca->i2c_num, pca->i2c_address, data, sizeof(data), pdMS_TO_TICKS(I2C_TIMEOUT_MS)) == ESP_OK)
        {
            break;
        }
        ESP_LOGW(TAG, "Write direction reg failed, retry %d/%d", i + 1, I2C_TRY_NUM);
        ESP_RETURN_ON_FALSE(i < I2C_TRY_NUM - 1, ESP_ERR_INVALID_STATE, TAG, "Write direction reg failed");
    }
    pca->regs.direction = value;
    return ESP_OK;
}

static esp_err_t read_direction_reg(esp_io_expander_handle_t handle, uint32_t *value)
{
    esp_io_expander_pca95xx_16bit_t *pca = (esp_io_expander_pca95xx_16bit_t *)__containerof(handle, esp_io_expander_pca95xx_16bit_t, base);

    *value = pca->regs.direction;
    return ESP_OK;
}

static esp_err_t reset(esp_io_expander_t *handle)
{
    esp_io_expander_pca95xx_16bit_t *pca = (esp_io_expander_pca95xx_16bit_t *)__containerof(handle, esp_io_expander_pca95xx_16bit_t, base);
    pca->need_update = true;
    ESP_RETURN_ON_ERROR(write_direction_reg(handle, DIR_REG_DEFAULT_VAL), TAG, "Write dir reg failed");
    ESP_RETURN_ON_ERROR(write_output_reg(handle, OUT_REG_DEFAULT_VAL), TAG, "Write output reg failed");
    return ESP_OK;
}

static esp_err_t del(esp_io_expander_t *handle)
{
    esp_io_expander_pca95xx_16bit_t *pca = (esp_io_expander_pca95xx_16bit_t *)__containerof(handle, esp_io_expander_pca95xx_16bit_t, base);
    if (pca->int_gpio != -1)
    {
        gpio_intr_disable(pca->int_gpio);
        gpio_reset_pin(pca->int_gpio);
    }

    free(pca);
    return ESP_OK;
}