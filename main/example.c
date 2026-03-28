#include <string.h>
#include "esp_log.h"
#include "livekit.h"
#include "livekit_sandbox.h"
#include "media.h"
#include "board.h"
#include "example.h"
#include "screen.h"

static const char *TAG = "livekit_example";

static livekit_room_handle_t room_handle;

/// Called when a transcription text stream is opened.
static void on_transcription_open(const livekit_data_stream_header_t *header, void *ctx)
{
    ESP_LOGI(TAG, "Transcription stream opened: stream_id=%s sender=%s",
             header->stream_id, header->sender_identity);
}

/// Called for each received transcription text chunk.
static void on_transcription_recv(const livekit_data_stream_chunk_t *chunk, void *ctx)
{
    const char *start = (const char *)chunk->content;
    const char *end = start + chunk->content_size;

    // Trim leading whitespace.
    while (start < end && (*start == ' ' || *start == '\t' || *start == '\n' || *start == '\r'))
        start++;
    // Trim trailing whitespace.
    while (end > start && (*(end - 1) == ' ' || *(end - 1) == '\t' || *(end - 1) == '\n' || *(end - 1) == '\r'))
        end--;

    int len = end - start;
    if (len == 0)
        return;

    // Skip chunks with more than one word (full-sentence repeats).
    for (const char *p = start; p < end; p++)
    {
        if (*p == ' ' || *p == '\t')
        {
            screen_show_text("");
            return;
        }
    }

    char buf[256];
    if (len > (int)sizeof(buf) - 1)
        len = sizeof(buf) - 1;
    memcpy(buf, start, len);
    buf[len] = '\0';

    ESP_LOGI(TAG, "Transcription: %s", buf);
    screen_show_text(buf);
}

/// Called when a transcription text stream is closed.
static void on_transcription_close(const livekit_data_stream_trailer_t *trailer, void *ctx)
{
    ESP_LOGI(TAG, "Transcription stream closed: stream_id=%s reason=%s",
             trailer->stream_id, trailer->reason);
}

/// Invoked when the room's connection state changes.
static void on_state_changed(livekit_connection_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "Room state changed: %s", livekit_connection_state_str(state));

    livekit_failure_reason_t reason = livekit_room_get_failure_reason(room_handle);
    if (reason != LIVEKIT_FAILURE_REASON_NONE)
    {
        ESP_LOGE(TAG, "Failure reason: %s", livekit_failure_reason_str(reason));
    }
}

/// Push-to-talk: unmute mic while button is held.
static void on_btn_press(void)
{
    ESP_LOGI(TAG, "Button pressed - mic unmuted");
    media_set_mic_muted(false);
    screen_show_text("Listening...");
}

static void on_btn_release(void)
{
    ESP_LOGI(TAG, "Button released - mic muted");
    media_set_mic_muted(true);
}

void join_room()
{
    if (room_handle != NULL)
    {
        ESP_LOGE(TAG, "Room already created");
        return;
    }

    livekit_room_options_t room_options = {
        .publish = {
            .kind = LIVEKIT_MEDIA_TYPE_AUDIO,
            .audio_encode = {
                .codec = LIVEKIT_AUDIO_CODEC_OPUS,
                .sample_rate = 16000,
                .channel_count = 1},
            .capturer = media_get_capturer()},
        .subscribe = {.kind = LIVEKIT_MEDIA_TYPE_AUDIO, .renderer = media_get_renderer()},
        .on_state_changed = on_state_changed};
    if (livekit_room_create(&room_handle, &room_options) != LIVEKIT_ERR_NONE)
    {
        ESP_LOGE(TAG, "Failed to create room");
        return;
    }

    livekit_data_stream_handler_t transcription_handler = {
        .on_recv = on_transcription_recv,
        .on_open = on_transcription_open,
        .on_close = on_transcription_close,
    };
    if (livekit_room_on_data_stream(room_handle, "lk.transcription", &transcription_handler) != LIVEKIT_ERR_NONE)
    {
        ESP_LOGE(TAG, "Failed to register transcription stream handler");
    }

    livekit_err_t connect_res;
#ifdef CONFIG_LK_EXAMPLE_USE_SANDBOX
    // Option A: Sandbox token server.
    livekit_sandbox_res_t res = {};
    livekit_sandbox_options_t gen_options = {
        .sandbox_id = CONFIG_LK_EXAMPLE_SANDBOX_ID,
        .room_name = CONFIG_LK_EXAMPLE_ROOM_NAME,
        .participant_name = CONFIG_LK_EXAMPLE_PARTICIPANT_NAME};
    if (!livekit_sandbox_generate(&gen_options, &res))
    {
        ESP_LOGE(TAG, "Failed to generate sandbox token");
        return;
    }
    connect_res = livekit_room_connect(room_handle, res.server_url, res.token);
    livekit_sandbox_res_free(&res);
#else
    // Option B: Pre-generated token.
    connect_res = livekit_room_connect(
        room_handle,
        CONFIG_LK_EXAMPLE_SERVER_URL,
        CONFIG_LK_EXAMPLE_TOKEN);
#endif

    if (connect_res != LIVEKIT_ERR_NONE)
    {
        ESP_LOGE(TAG, "Failed to connect to room");
    }

    // Push-to-talk: start with mic muted, unmute while button is held.
    media_set_mic_muted(true);
    board_set_btn_press_cb(on_btn_press);
    board_set_btn_release_cb(on_btn_release);
}

void leave_room()
{
    if (room_handle == NULL)
    {
        ESP_LOGE(TAG, "Room not created");
        return;
    }
    if (livekit_room_close(room_handle) != LIVEKIT_ERR_NONE)
    {
        ESP_LOGE(TAG, "Failed to leave room");
    }
    if (livekit_room_destroy(room_handle) != LIVEKIT_ERR_NONE)
    {
        ESP_LOGE(TAG, "Failed to destroy room");
        return;
    }
    room_handle = NULL;
}