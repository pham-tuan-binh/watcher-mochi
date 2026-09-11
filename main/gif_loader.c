#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensecap-watcher.h"
#include "gif_loader.h"

static const char *TAG = "gif_loader";

#define SD_MOUNT_POINT "/sdcard"
#define MAX_GIFS       100
#define READ_CHUNK_SIZE (64 * 1024)

static char *s_gif_files[MAX_GIFS];
static int s_gif_count;

static uint8_t *s_blank_data;
static size_t s_blank_size;

/// DMA-capable bounce buffer reused across reads.
static uint8_t *s_dma_buf;

static TaskHandle_t s_loader_task;
static void (*s_on_loaded)(uint8_t *data, size_t size);

/// Previously loaded GIF data (freed on next load).
static uint8_t *s_prev_data;

static void scan_gif_files(void)
{
    DIR *dir = opendir(SD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open %s", SD_MOUNT_POINT);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_gif_count < MAX_GIFS) {
        size_t len = strlen(entry->d_name);
        if (len <= 4 || strcasecmp(entry->d_name + len - 4, ".gif") != 0)
            continue;
        if (strncmp(entry->d_name, "._", 2) == 0)
            continue;
        if (strcasecmp(entry->d_name, "blank.gif") == 0)
            continue;

        char path[256];
        snprintf(path, sizeof(path), SD_MOUNT_POINT "/%s", entry->d_name);
        s_gif_files[s_gif_count] = strdup(path);
        if (s_gif_files[s_gif_count]) {
            ESP_LOGI(TAG, "Found: %s", s_gif_files[s_gif_count]);
            s_gif_count++;
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "%d GIF files found", s_gif_count);
}

/// Read a file into SPIRAM via DMA bounce buffer. Returns size or 0 on failure.
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

/// Try to read a file, remounting the SD card on failure (SPI bus recovery).
static size_t read_file_with_retry(const char *path, uint8_t **out)
{
    size_t size = read_file(path, out);
    if (size > 0)
        return size;

    ESP_LOGW(TAG, "Read failed, remounting SD card");
    bsp_sdcard_deinit_default();
    vTaskDelay(pdMS_TO_TICKS(200));
    if (bsp_sdcard_init_default() != ESP_OK)
        return 0;

    return read_file(path, out);
}

static void loader_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (s_gif_count == 0)
            continue;

        int idx = esp_random() % s_gif_count;
        ESP_LOGI(TAG, "Loading: %s", s_gif_files[idx]);

        uint8_t *data = NULL;
        size_t size = read_file_with_retry(s_gif_files[idx], &data);

        free(s_prev_data);
        s_prev_data = data;

        if (s_on_loaded)
            s_on_loaded(data, size);
    }
}

int gif_loader_init(void)
{
    esp_err_t err = bsp_sdcard_init_default();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(err));
        return 0;
    }

    scan_gif_files();

    s_dma_buf = heap_caps_malloc(READ_CHUNK_SIZE, MALLOC_CAP_DMA);
    s_blank_size = read_file(SD_MOUNT_POINT "/blank.gif", &s_blank_data);
    if (!s_blank_data)
        ESP_LOGW(TAG, "blank.gif not found on SD card");

    xTaskCreate(loader_task, "gif_loader", 4096, NULL, 5, &s_loader_task);

    return s_gif_count;
}

const uint8_t *gif_loader_get_blank(size_t *out_size)
{
    if (out_size)
        *out_size = s_blank_size;
    return s_blank_data;
}

void gif_loader_request(void (*cb)(uint8_t *data, size_t size))
{
    if (s_gif_count == 0)
        return;
    s_on_loaded = cb;
    xTaskNotifyGive(s_loader_task);
}
