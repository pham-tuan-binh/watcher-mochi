/**
 * Pond sound design, synthesised on the fly — there are no audio assets.
 *
 * Every sound is one sine oscillator whose pitch glides upwards while its
 * amplitude decays. That rising chirp is what makes a water droplet read as
 * a droplet (the bubble left behind by the drop shrinks, so its resonance
 * climbs). Randomising the starting pitch per hit keeps a pond of them from
 * sounding mechanical.
 *
 * Voices are mixed in a single task and fed through two damped feedback
 * combs, which is just enough reverb to put the pond in a dark room. The
 * task only streams while something is sounding, so silence costs nothing.
 */

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sensecap-watcher.h"
#include "sound.h"

static const char *TAG = "sound";

#define SR            DRV_AUDIO_SAMPLE_RATE   /* 16 kHz, mono, 16-bit */
#define BLOCK         256                     /* 16 ms per write       */
#define MAX_VOICES    4
#define MASTER_VOLUME 70
#define ATTACK        24                      /* samples, kills the click */

/* Two feedback combs at 77 ms and 108 ms, damped in the loop. */
#define COMB_A 1231
#define COMB_B 1723
#define TAIL_BLOCKS (SR / BLOCK)              /* render ~1 s of tail out */

// --- Voice presets ---

typedef struct {
    uint16_t f0_lo, f0_hi;  /* starting pitch is picked in this range, Hz */
    uint16_t rise;          /* pitch it glides to, as a percent of f0     */
    uint16_t sweep_ms;      /* how quickly it gets there                  */
    uint16_t decay_ms;      /* amplitude decay time constant              */
    uint16_t len_ms;
    uint8_t  amp;           /* 0..255 */
    uint8_t  send;          /* into the reverb, 0..255 */
} preset_t;

static const preset_t PRESETS[SOUND_COUNT] = {
    /* DROP    */ {  620,  820, 260, 14,  70, 210, 140, 190 },
    /* SURFACE */ {  280,  380, 220, 26, 130, 290,  62, 150 },
    /* DISTANT */ {  240,  320, 180, 32, 120, 280,  30, 225 },
    /* TICK    */ { 1380, 1500, 100,  1,   8,  32,  42,   0 },
};

// --- Oscillator table ---

#define LUT 512
static float s_sine[LUT + 1];

static inline float osc(float phase)
{
    float f = phase * LUT;
    int i = (int)f;
    return s_sine[i] + (s_sine[i + 1] - s_sine[i]) * (f - (float)i);
}

// --- State ---

typedef struct {
    bool active;
    uint32_t left;      /* samples remaining */
    uint16_t attack;
    float phase;        /* 0..1 */
    float f_end, f_gap, f_k;
    float env, env_k;
    float send;
} voice_t;

static voice_t s_voices[MAX_VOICES];

static float s_ca[COMB_A], s_cb[COMB_B];
static int s_ia, s_ib;
static float s_lpa, s_lpb;

static int16_t s_block[BLOCK];
static QueueHandle_t s_queue;

static float rnd_hz(uint16_t lo, uint16_t hi)
{
    if (hi <= lo)
        return (float)lo;
    return (float)lo + (float)(esp_random() % (uint32_t)(hi - lo + 1));
}

/// Exponential per-sample multiplier that decays to 1/e in `ms`.
static float decay_k(uint16_t ms)
{
    float samples = (float)ms * (float)SR / 1000.0f;
    if (samples < 1.0f)
        samples = 1.0f;
    return expf(-1.0f / samples);
}

static void voice_start(sound_t which)
{
    if (which < 0 || which >= SOUND_COUNT)
        return;
    const preset_t *p = &PRESETS[which];

    /* steal the quietest voice if they are all busy */
    voice_t *v = NULL;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!s_voices[i].active) {
            v = &s_voices[i];
            break;
        }
        if (!v || s_voices[i].env < v->env)
            v = &s_voices[i];
    }

    float f0 = rnd_hz(p->f0_lo, p->f0_hi);
    float f1 = f0 * (float)p->rise / 100.0f;

    /* vary the decay too, so the drops sound like different sized drops */
    uint16_t decay = (uint16_t)((uint32_t)p->decay_ms * (84 + esp_random() % 37) / 100);

    v->active = true;
    v->left = (uint32_t)p->len_ms * SR / 1000;
    v->attack = ATTACK;
    v->phase = 0.0f;
    v->f_end = f1;
    v->f_gap = f1 - f0;
    v->f_k = decay_k(p->sweep_ms);
    v->env = (float)p->amp / 255.0f;
    v->env_k = decay_k(decay);
    v->send = (float)p->send / 255.0f;
}

static bool voices_active(void)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (s_voices[i].active)
            return true;
    return false;
}

static void reverb_reset(void)
{
    memset(s_ca, 0, sizeof s_ca);
    memset(s_cb, 0, sizeof s_cb);
    s_lpa = s_lpb = 0.0f;
    s_ia = s_ib = 0;
}

static void render_block(bool fade)
{
    for (int n = 0; n < BLOCK; n++) {
        float dry = 0.0f, wet = 0.0f;

        for (int i = 0; i < MAX_VOICES; i++) {
            voice_t *v = &s_voices[i];
            if (!v->active)
                continue;

            float f = v->f_end - v->f_gap;
            v->f_gap *= v->f_k;

            v->phase += f * (1.0f / (float)SR);
            if (v->phase >= 1.0f)
                v->phase -= 1.0f;

            float s = osc(v->phase) * v->env;
            v->env *= v->env_k;
            if (v->attack) {
                s *= (float)(ATTACK - v->attack) / (float)ATTACK;
                v->attack--;
            }

            dry += s;
            wet += s * v->send;
            if (--v->left == 0)
                v->active = false;
        }

        float ya = s_ca[s_ia], yb = s_cb[s_ib];
        s_lpa += (ya - s_lpa) * 0.45f;
        s_lpb += (yb - s_lpb) * 0.36f;
        s_ca[s_ia] = wet + s_lpa * 0.55f;
        s_cb[s_ib] = wet + s_lpb * 0.50f;
        if (++s_ia >= COMB_A) s_ia = 0;
        if (++s_ib >= COMB_B) s_ib = 0;

        float out = dry + (ya + yb) * 0.32f;
        if (fade)
            out *= (float)(BLOCK - n) / (float)BLOCK;

        /* soft limiter: near linear for one drop, bends instead of clipping
         * when several land at once */
        out = tanhf(out * 0.9f);

        s_block[n] = (int16_t)(out * 29000.0f);
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    bsp_codec_volume_set(MASTER_VOLUME, NULL);

    int tail = 0;
    for (;;) {
        sound_t kind;

        /* nothing sounding and the room has gone quiet: sleep until poked */
        if (!voices_active() && tail <= 0) {
            if (xQueueReceive(s_queue, &kind, portMAX_DELAY) != pdTRUE)
                continue;
            voice_start(kind);
        }
        while (xQueueReceive(s_queue, &kind, 0) == pdTRUE)
            voice_start(kind);

        bool fade = false;
        if (voices_active()) {
            tail = TAIL_BLOCKS;
        } else if (--tail <= 0) {
            tail = 0;
            fade = true;          /* last block, ramp out so it cannot click */
        }

        render_block(fade);

        size_t written;
        bsp_i2s_write(s_block, sizeof s_block, &written, 200);

        if (fade)
            reverb_reset();
    }
}

// --- Public API ---

void sound_init(void)
{
    if (s_queue)
        return;

    for (int i = 0; i <= LUT; i++)
        s_sine[i] = sinf((float)i * 2.0f * (float)M_PI / (float)LUT);

    s_queue = xQueueCreate(8, sizeof(sound_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create sound queue");
        return;
    }

    if (xTaskCreate(audio_task, "audio", 4096, NULL, 5, NULL) != pdPASS)
        ESP_LOGE(TAG, "Failed to start audio task");
}

void sound_play(sound_t which)
{
    if (!s_queue)
        return;
    xQueueSend(s_queue, &which, 0);
}
