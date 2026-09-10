/**
 * Pond sound design, synthesised on the fly — there are no audio assets.
 *
 * The plop of something hitting water is not the drop itself. Phillips,
 * Agarwal and Jordan filmed it (Scientific Reports, 2018) and found the
 * sound is driven by a small air bubble trapped under the surface: the
 * impact makes a brief click, the crater takes a few milliseconds to form,
 * and then the entrapped bubble rings and drives the surface like a piston.
 * https://www.nature.com/articles/s41598-018-27913-0
 *
 * So each voice here is that same three-part event:
 *
 *   1. a short filtered noise click for the impact,
 *   2. a few milliseconds of nothing while the crater forms,
 *   3. a decaying sine at the bubble's resonance.
 *
 * The pitch comes from Minnaert's 1933 result that a bubble in water
 * resonates at f0 * r ~= 3.26 Hz*m, so voices are specified by bubble
 * radius rather than by frequency and the pitch follows from the physics.
 * https://en.wikipedia.org/wiki/Minnaert_resonance
 *
 * The pitch also rises as it rings, which is the part your ear reads as
 * "water". Van den Doel's liquid sound model (ACM TAP, 2005) captures it as
 * f(t) = f0 * (1 + XI * d * t) against an exp(-d * t) decay, with XI ~= 0.1
 * found experimentally. That works out to roughly a 3 * XI rise over the
 * audible life of the bubble whatever the damping, and it costs one add per
 * sample. https://dl.acm.org/doi/10.1145/1101530.1101554
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

/* Minnaert: f0 * r ~= 3.26 Hz*m, so f0 = 3.26e6 / r for r in micrometres. */
#define MINNAERT 3260000.0f

/* Van den Doel's rise constant. 0.1 is the measured value; turning it up
 * exaggerates the "wet" chirp, which some tiny speakers need. */
#ifndef BUBBLE_XI
#define BUBBLE_XI 0.10f
#endif

/* Two feedback combs at 77 ms and 108 ms, damped in the loop. */
#define COMB_A 1231
#define COMB_B 1723
#define TAIL_BLOCKS (SR / BLOCK)              /* render ~1 s of tail out */

// --- Voice presets ---

typedef struct {
    uint16_t r_lo, r_hi;    /* entrapped bubble radius, micrometres */
    uint16_t damping;       /* d, per second */
    uint16_t len_ms;
    uint8_t  amp;           /* bubble tone level, 0 = impact click only */
    uint8_t  send;          /* into the reverb, 0..255 */
    uint8_t  click;         /* impact transient level, 0..255 */
    uint8_t  click_ms;      /* impact transient decay */
    uint8_t  click_lp;      /* impact brightness: one-pole coeff, 0..255 */
    uint8_t  delay_ms;      /* crater forming, before the bubble rings */
} preset_t;

/* 3000-4200 um is roughly 780-1090 Hz, about a fingertip's worth of trapped
 * air; the ambient voices use bigger, lazier bubbles further down. A real
 * tap drip traps a bubble ten times smaller and plinks near 9 kHz, which
 * this 16 kHz codec and its little speaker could not reproduce anyway. */
static const preset_t PRESETS[SOUND_COUNT] = {
    /* DROP    */ { 3000, 4200, 30, 200, 150, 190, 110, 4, 140,  7 },
    /* SURFACE */ { 5000, 7000, 22, 280,  70, 150,  45, 6,  90, 10 },
    /* DISTANT */ { 6000, 9000, 20, 280,  34, 230,  20, 7,  70, 12 },
    /* TICK    */ { 4000, 4000, 30,  40,   0,   0,  95, 3, 205,  0 },
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

static inline float noise(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return (float)(int32_t)x * (1.0f / 2147483648.0f);
}

// --- State ---

typedef struct {
    bool active;
    uint32_t left;      /* samples remaining */
    uint32_t delay;     /* samples before the bubble starts ringing */
    float phase;        /* 0..1 */
    float freq, dfreq;  /* the rising resonance and its per-sample step */
    float env, env_k;
    float click, click_k, click_lp, lp;
    uint32_t rng;
    float send;
} voice_t;

static voice_t s_voices[MAX_VOICES];

static float s_ca[COMB_A], s_cb[COMB_B];
static int s_ia, s_ib;
static float s_lpa, s_lpb;

static int16_t s_block[BLOCK];
static QueueHandle_t s_queue;

/// Exponential per-sample multiplier that decays to 1/e in `ms`.
static float decay_ms_k(float ms)
{
    float samples = ms * (float)SR / 1000.0f;
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

    uint32_t r = p->r_lo;
    if (p->r_hi > p->r_lo)
        r += esp_random() % (uint32_t)(p->r_hi - p->r_lo + 1);
    float f0 = MINNAERT / (float)r;
    float d = (float)p->damping;

    v->active = true;
    v->left = (uint32_t)p->len_ms * SR / 1000;
    v->delay = (uint32_t)p->delay_ms * SR / 1000;
    v->phase = 0.0f;
    v->freq = f0;
    v->dfreq = f0 * BUBBLE_XI * d / (float)SR;
    v->env = (float)p->amp / 255.0f;
    v->env_k = expf(-d / (float)SR);
    v->click = (float)p->click / 255.0f;
    v->click_k = decay_ms_k((float)p->click_ms);
    v->click_lp = (float)p->click_lp / 255.0f;
    v->lp = 0.0f;
    v->rng = esp_random() | 1u;
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

            float s = 0.0f;

            /* the impact itself: a short, dull noise burst */
            if (v->click > 0.0005f) {
                v->lp += (noise(&v->rng) - v->lp) * v->click_lp;
                s += v->lp * v->click;
                v->click *= v->click_k;
            }

            /* then, once the crater has formed, the trapped bubble rings */
            if (v->delay) {
                v->delay--;
            } else if (v->env > 0.0f) {
                v->phase += v->freq * (1.0f / (float)SR);
                if (v->phase >= 1.0f)
                    v->phase -= 1.0f;
                s += osc(v->phase) * v->env;
                v->freq += v->dfreq;      /* f(t) = f0 (1 + XI d t) */
                v->env *= v->env_k;
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
