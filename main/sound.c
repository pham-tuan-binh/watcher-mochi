#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensecap-watcher.h"
#include "pop_sound.h"
#include "sound.h"

#define POP_VOLUME 60

/// Pitch shift range: 236–276 maps to roughly ±8% variation around 256 (1:1).
#define PITCH_SHIFT_BASE 236
#define PITCH_SHIFT_RANGE 41

static int16_t s_pop_buf[POP_SOUND_SAMPLES];
static TaskHandle_t s_pop_task;

static void pop_task(void *arg)
{
    (void)arg;
    bsp_codec_volume_set(POP_VOLUME, NULL);

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int pitch_shift = PITCH_SHIFT_BASE + (int)(esp_random() % PITCH_SHIFT_RANGE);

        int out_len = 0;
        for (int src = 0; src < POP_SOUND_SAMPLES && out_len < POP_SOUND_SAMPLES; ) {
            s_pop_buf[out_len++] = pop_sound_data[src];
            src = (int)((int64_t)out_len * pitch_shift / 256);
        }

        size_t written;
        bsp_i2s_write(s_pop_buf, out_len * sizeof(int16_t), &written, 500);
    }
}

void sound_play_pop(void)
{
    if (!s_pop_task) {
        xTaskCreate(pop_task, "pop", 4096, NULL, 5, &s_pop_task);
    }
    xTaskNotifyGive(s_pop_task);
}
