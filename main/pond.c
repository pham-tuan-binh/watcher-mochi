/**
 * A small pixel-art koi pond, lit like a cave.
 *
 * Everything is simulated on a low resolution grid (GRID_W x GRID_H) and
 * blown up by PIX in both directions into an LVGL canvas. Two buffers are
 * kept per grid pixel: a material (water, pad, koi, ...) and a light level.
 * Colour only comes together at blit time, where each material is looked up
 * in its own dark-to-lit ramp. Anything that glows just adds light, so the
 * koi, the ripples and the motes all sit in the same lighting model.
 *
 * Per frame: ambient water light -> koi glow -> ripple light -> koi bodies
 * -> lily pads -> motes -> blit. Glow is only added to open water so a ring
 * never washes out a fish or a pad.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "lvgl.h"
#include "sensecap-watcher.h"
#include "pond.h"

static const char *TAG = "pond";

// --- Geometry ---

#define PIX      4                        /* screen pixels per pond pixel */
#define GRID_W   (DRV_LCD_H_RES / PIX)
#define GRID_H   (DRV_LCD_V_RES / PIX)
#define GRID_CX  (GRID_W / 2)
#define GRID_CY  (GRID_H / 2)
#define POND_R   (GRID_W / 2)             /* the display is round */
#define FRAME_MS 40

#define KOI_COUNT   CONFIG_MOCHI_POND_KOI_COUNT
#define PAD_COUNT   CONFIG_MOCHI_POND_LILY_COUNT
#define MOTE_COUNT  CONFIG_MOCHI_POND_MOTE_COUNT
#define MAX_RIPPLES 12

// --- Materials and their light ramps ---

enum {
    MAT_WATER = 0,
    MAT_PAD, MAT_PAD_RIM, MAT_FLOWER,
    MAT_KOI_A_M, MAT_KOI_A_A, MAT_KOI_A_F,
    MAT_KOI_B_M, MAT_KOI_B_A, MAT_KOI_B_F,
    MAT_KOI_C_M, MAT_KOI_C_A, MAT_KOI_C_F,
    MAT_MOTE,
    MAT_COUNT
};

#define LEVELS 6

/* Fully lit colour of each material. Everything fades towards NIGHT. */
static const uint8_t MAT_RGB[MAT_COUNT][3] = {
    {  70, 150, 165 },                                   /* water          */
    {  74, 140,  70 }, { 156, 204, 112 }, { 245, 175, 205 },
    { 255, 138,  48 }, { 250, 246, 240 }, { 236, 182, 150 },
    { 238,  70,  70 }, { 248, 242, 238 }, { 232, 154, 154 },
    { 120, 132, 160 }, { 215, 222, 235 }, { 168, 186, 208 },
    { 205, 245, 225 },                                   /* drifting mote  */
};

static const uint8_t NIGHT_RGB[3] = { 4, 8, 13 };
static const uint8_t GLOW_RGB[3]  = { 165, 225, 235 };

/* How much of the base colour survives at each light level, in 1/256ths. */
static const uint16_t LEVEL_MIX[LEVELS] = { 26, 56, 100, 150, 205, 256 };

static lv_color_t s_pal[MAT_COUNT * LEVELS];

// --- Koi sprite ---
// Body space: x runs tail (0) -> nose (KOI_W - 1), y is across the body.
// 'm' main colour, 'a' accent patch, 'f' tail, '.' water.

#define KOI_W 11
#define KOI_H 7
#define KOI_PIVOT 5    /* body-space x the koi turns about */
#define KOI_REACH 8    /* body bounding box half-size on the grid */
#define KOI_GLOW_R 10  /* halo cast on the surrounding water */

static const char *const KOI_ART[KOI_H] = {
    "...........",
    "ff.........",
    "fffmmmaamm.",
    "ffmmmaaammm",
    "fffmmmaamm.",
    "ff.........",
    "...........",
};

static uint8_t s_koi_sprite[KOI_H][KOI_W];

/* main, accent, tail — indexed by sprite value - 1 */
static const uint8_t KOI_MAT[3][3] = {
    { MAT_KOI_A_M, MAT_KOI_A_A, MAT_KOI_A_F },
    { MAT_KOI_B_M, MAT_KOI_B_A, MAT_KOI_B_F },
    { MAT_KOI_C_M, MAT_KOI_C_A, MAT_KOI_C_F },
};

// --- Trig, all fixed point (256 = 1.0, angles are 0..255 = full turn) ---

static int16_t s_sin[256];

#define SIN(a) (s_sin[(uint8_t)(a)])
#define COS(a) (s_sin[(uint8_t)((a) + 64)])

static void trig_init(void)
{
    for (int i = 0; i < 256; i++)
        s_sin[i] = (int16_t)lroundf(sinf((float)i * 2.0f * (float)M_PI / 256.0f) * 256.0f);
}

static uint8_t angle_of(int dx, int dy)
{
    float a = atan2f((float)dy, (float)dx) * 256.0f / (2.0f * (float)M_PI);
    return (uint8_t)(int)lroundf(a);
}

static int isqrt32(int32_t v)
{
    if (v <= 0)
        return 0;
    int32_t x = v, y = (x + 1) / 2;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2;
    }
    return (int)x;
}

static uint32_t rnd(uint32_t n)
{
    return esp_random() % n;
}

// --- Entities ---

typedef struct {
    int32_t x, y;      /* grid position, 8.8 fixed point */
    uint8_t heading;   /* 0..255 */
    uint8_t phase;     /* tail wiggle phase */
    int16_t speed;     /* 8.8 grid pixels per frame */
    int8_t  turn;
    uint8_t type;
    uint16_t boost;    /* frames left chasing the last tap */
    int16_t tx, ty;    /* what the koi is swimming towards */
} koi_t;

typedef struct {
    int16_t cx, cy;
    uint8_t r;
    uint8_t notch;     /* direction of the wedge cut out of the pad */
    uint8_t phase;     /* bob phase */
    bool flower;
} pad_t;

typedef struct {
    int16_t x, y;      /* 8.8 fixed point */
    uint8_t heading;
    uint8_t phase;
    int16_t speed;
} mote_t;

typedef struct {
    int16_t x, y;
    int16_t age;       /* negative while the ripple is still waiting to start */
    int16_t life;
    bool active;
} ripple_t;

#define RIPPLE_SPEED 340   /* 8.8 grid pixels per frame */
#define RIPPLE_LIFE  46

static koi_t s_koi[KOI_COUNT];
#if PAD_COUNT > 0
static pad_t s_pads[PAD_COUNT];
#endif
#if MOTE_COUNT > 0
static mote_t s_motes[MOTE_COUNT];
#endif
static ripple_t s_ripples[MAX_RIPPLES];

static uint8_t s_mat[GRID_H][GRID_W];
static int16_t s_light[GRID_H][GRID_W];
static int8_t s_dither[4][4];

static lv_color_t s_row[DRV_LCD_H_RES];
static lv_color_t *s_canvas_buf;
static lv_obj_t *s_canvas;
static uint32_t s_frame;
static uint16_t s_next_ambient;

/// Light stays in 0..255; blit quantises it into LEVELS steps.
#define LIGHT_MAX 255

static inline void light_add(int gx, int gy, int amount)
{
    int v = s_light[gy][gx] + amount;
    if (v < 0) v = 0;
    if (v > LIGHT_MAX) v = LIGHT_MAX;
    s_light[gy][gx] = (int16_t)v;
}

// --- Water ---

#define AMBIENT_CORE 120   /* light on open water at the centre of the pond */

static void draw_water(void)
{
    int a1 = (int)s_frame;
    int a2 = -2 * (int)s_frame;
    int a3 = 3 * (int)s_frame;
    const int r2max = POND_R * POND_R;

    for (int y = 0; y < GRID_H; y++) {
        int dy = y - GRID_CY;
        int dy2 = dy * dy;
        uint8_t *mrow = s_mat[y];
        int16_t *lrow = s_light[y];
        for (int x = 0; x < GRID_W; x++) {
            int dx = x - GRID_CX;
            int d2 = dx * dx + dy2;

            /* slow crossing swells, quantised later into dithered bands */
            int w = SIN(x * 6 + y * 3 + a1)
                  + SIN(x * 3 - y * 7 + a2)
                  + (SIN(x * 11 + y * 9 + a3) >> 1);

            /* light drops off towards the rim: the pond edge is nearly black,
             * with a slight lean towards the upper left */
            int l = AMBIENT_CORE - (AMBIENT_CORE * d2) / r2max + (w >> 4)
                  - (dx + dy) / 3;
            if (l < 0) l = 0;

            mrow[x] = MAT_WATER;
            lrow[x] = (int16_t)l;
        }
    }
}

// --- Ripples ---

static void ripple_spawn(int gx, int gy, int delay, int life)
{
    for (int i = 0; i < MAX_RIPPLES; i++) {
        if (s_ripples[i].active)
            continue;
        s_ripples[i].x = (int16_t)gx;
        s_ripples[i].y = (int16_t)gy;
        s_ripples[i].age = (int16_t)-delay;
        s_ripples[i].life = (int16_t)life;
        s_ripples[i].active = true;
        return;
    }
}

static void ripples_update(void)
{
    for (int i = 0; i < MAX_RIPPLES; i++) {
        if (!s_ripples[i].active)
            continue;
        if (++s_ripples[i].age >= s_ripples[i].life)
            s_ripples[i].active = false;
    }
}

/// One ring per ripple: a lit crest with a dark trough trailing behind it.
static void ripples_draw(void)
{
    for (int i = 0; i < MAX_RIPPLES; i++) {
        const ripple_t *rp = &s_ripples[i];
        if (!rp->active || rp->age <= 0)
            continue;

        int rad = (rp->age * RIPPLE_SPEED) >> 8;
        if (rad < 1)
            continue;

        /* fade as the ring spreads out */
        int rem = rp->life - rp->age;
        int str = 3;
        if (rem < rp->life / 3)          str = 1;
        else if (rem < rp->life * 2 / 3) str = 2;

        int lo = rad - 4;
        if (lo < 0)
            lo = 0;
        int hi = rad + 4;
        int lo2 = lo * lo, hi2 = hi * hi, r2 = rad * rad;
        int den = 2 * rad;

        int y0 = rp->y - hi, y1 = rp->y + hi;
        if (y0 < 0) y0 = 0;
        if (y1 >= GRID_H) y1 = GRID_H - 1;

        for (int y = y0; y <= y1; y++) {
            int dy = y - rp->y;
            int dy2 = dy * dy;
            if (dy2 > hi2)
                continue;
            int half = isqrt32(hi2 - dy2);
            int x0 = rp->x - half, x1 = rp->x + half;
            if (x0 < 0) x0 = 0;
            if (x1 >= GRID_W) x1 = GRID_W - 1;

            const uint8_t *mrow = s_mat[y];
            for (int x = x0; x <= x1; x++) {
                if (mrow[x] != MAT_WATER)
                    continue;
                int dx = x - rp->x;
                int d2 = dx * dx + dy2;
                if (d2 < lo2)
                    continue;

                /* (d2 - r2) / 2r approximates the signed distance to the ring */
                int k = (d2 - r2) / den;
                if (k >= -1 && k <= 1)
                    light_add(x, y, str * 34);
                else if (k == -2 || k == 2)
                    light_add(x, y, str * 12);
                else if (k > 2 && str >= 2)
                    light_add(x, y, -22);
            }
        }
    }
}

// --- Lily pads ---

#if PAD_COUNT > 0
static void pad_place(pad_t *p, int idx)
{
    p->r = (uint8_t)(6 + rnd(4));
    p->notch = (uint8_t)rnd(256);
    p->phase = (uint8_t)rnd(256);
    p->flower = (p->r >= 7) && (rnd(3) == 0);

    for (int tries = 0; tries < 80; tries++) {
        int reach = POND_R - p->r - 3;
        int a = (int)rnd(256);
        /* pick the radius by area so the pads do not bunch up in the middle */
        int d = isqrt32((int32_t)rnd((uint32_t)(reach * reach) + 1));
        p->cx = (int16_t)(GRID_CX + ((COS(a) * d) >> 8));
        p->cy = (int16_t)(GRID_CY + ((SIN(a) * d) >> 8));

        bool clear = true;
        for (int j = 0; j < idx; j++) {
            int dx = p->cx - s_pads[j].cx;
            int dy = p->cy - s_pads[j].cy;
            int min = p->r + s_pads[j].r + 4;
            if (dx * dx + dy * dy < min * min) {
                clear = false;
                break;
            }
        }
        if (clear)
            return;
    }
}

/// Pads read as dark silhouettes with a lit rim on the side facing the light.
static void draw_pad(const pad_t *p)
{
    int cx = p->cx;
    int cy = p->cy + (SIN(p->phase) >> 8);   /* gentle bob, +/- 1 pixel */
    int r = p->r;
    int r2 = r * r;
    int inner2 = (r - 1) * (r - 1);
    int ncs = COS(p->notch), nsn = SIN(p->notch);

    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= GRID_H)
            continue;
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx;
            if (x < 0 || x >= GRID_W)
                continue;
            int d2 = dx * dx + dy * dy;
            if (d2 > r2)
                continue;

            /* narrow wedge notch, cut in from the rim but not to the centre */
            int dot = (dx * ncs + dy * nsn) >> 8;
            int cross = (-dx * nsn + dy * ncs) >> 8;
            if (dot > r / 4 && abs(cross) * 5 < (dot - r / 4) * 2)
                continue;

            int lit = s_light[y][x];
            if (d2 >= inner2 && dx + dy < 0) {
                s_mat[y][x] = MAT_PAD_RIM;          /* catches the light */
                lit = lit + 90;
            } else {
                s_mat[y][x] = MAT_PAD;              /* in its own shadow */
                lit = (lit * 3) / 4 - 10 - (dx + dy) * 2;
            }
            if (lit < 0) lit = 0;
            if (lit > LIGHT_MAX) lit = LIGHT_MAX;
            s_light[y][x] = (int16_t)lit;
        }
    }

    if (!p->flower)
        return;

    /* a bright heart with four dim petals on the diagonals */
    static const int8_t PETALS[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
    for (int i = 0; i < 4; i++) {
        int x = cx + PETALS[i][0], y = cy + PETALS[i][1];
        if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H)
            continue;
        s_mat[y][x] = MAT_FLOWER;
        s_light[y][x] = 165;
    }
    if (cx >= 0 && cx < GRID_W && cy >= 0 && cy < GRID_H) {
        s_mat[cy][cx] = MAT_FLOWER;
        s_light[cy][cx] = LIGHT_MAX;
    }
}
#endif /* PAD_COUNT > 0 */

// --- Koi ---

static void koi_reset(koi_t *k, int i)
{
    int a = (int)rnd(256);
    int d = (int)rnd(POND_R - 12);
    k->x = (int32_t)(GRID_CX + ((COS(a) * d) >> 8)) << 8;
    k->y = (int32_t)(GRID_CY + ((SIN(a) * d) >> 8)) << 8;
    k->heading = (uint8_t)rnd(256);
    k->phase = (uint8_t)rnd(256);
    k->speed = (int16_t)(70 + rnd(40));
    k->turn = 0;
    k->type = (uint8_t)(i % 3);
    k->boost = 0;
}

static void koi_update(koi_t *k)
{
    int gx = k->x >> 8, gy = k->y >> 8;
    int dx = gx - GRID_CX, dy = gy - GRID_CY;
    int edge = POND_R - 9;
    int desired = -1;

    if (dx * dx + dy * dy > edge * edge) {
        desired = angle_of(-dx, -dy);        /* turn back towards the middle */
    } else if (k->boost) {
        int tdx = k->tx - gx, tdy = k->ty - gy;
        if (abs(tdx) + abs(tdy) < 5)
            k->boost = 0;
        else
            desired = angle_of(tdx, tdy);
    }

    if (desired >= 0) {
        int diff = (int8_t)((uint8_t)desired - k->heading);
        k->turn = (int8_t)(diff > 4 ? 4 : (diff < -4 ? -4 : diff));
    } else if (rnd(24) == 0) {
        k->turn = (int8_t)((int)rnd(5) - 2);
    }
    k->heading = (uint8_t)(k->heading + k->turn);

    int speed = k->speed;
    if (k->boost) {
        speed += speed / 2;
        k->boost--;
    }

    k->x += (COS(k->heading) * speed) >> 8;
    k->y += (SIN(k->heading) * speed) >> 8;
    k->phase = (uint8_t)(k->phase + 10 + (speed >> 4));
}

/// Soft halo the koi casts on the water around it.
static void draw_koi_glow(const koi_t *k)
{
    int cx = k->x >> 8, cy = k->y >> 8;
    const int r = KOI_GLOW_R;
    const int r2 = r * r;

    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= GRID_H)
            continue;
        int dy = y - cy, dy2 = dy * dy;
        if (dy2 > r2)
            continue;
        const uint8_t *mrow = s_mat[y];
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < 0 || x >= GRID_W)
                continue;
            if (mrow[x] != MAT_WATER)
                continue;
            int dx = x - cx;
            int d2 = dx * dx + dy2;
            if (d2 > r2)
                continue;
            light_add(x, y, (48 * (r2 - d2)) / r2);
        }
    }
}

static void draw_koi(const koi_t *k)
{
    int cx = k->x >> 8, cy = k->y >> 8;
    int cs = COS(k->heading), sn = SIN(k->heading);
    const uint8_t *mats = KOI_MAT[k->type];
    int wig_amp = SIN(k->phase);

    for (int y = cy - KOI_REACH; y <= cy + KOI_REACH; y++) {
        if (y < 0 || y >= GRID_H)
            continue;
        int dy = y - cy;
        for (int x = cx - KOI_REACH; x <= cx + KOI_REACH; x++) {
            if (x < 0 || x >= GRID_W)
                continue;
            int dx = x - cx;

            int bx = ((dx * cs + dy * sn) >> 8) + KOI_PIVOT;
            int by = ((-dx * sn + dy * cs) >> 8) + KOI_H / 2;
            if (bx < 0 || bx >= KOI_W)
                continue;

            /* the tail flicks, the head barely moves; keep the swing to one
             * pixel so the tail never tears away from the body */
            if (bx < 7)
                by -= (wig_amp * (7 - bx)) >> 10;
            if (by < 0 || by >= KOI_H)
                continue;

            uint8_t v = s_koi_sprite[by][bx];
            if (!v)
                continue;

            s_mat[y][x] = mats[v - 1];
            /* the body is what glows; the tail is thinner and half sunk */
            s_light[y][x] = (v == 3) ? 148 : 230;
        }
    }
}

// --- Drifting motes ---

#if MOTE_COUNT > 0
static void mote_reset(mote_t *m)
{
    int a = (int)rnd(256);
    int d = (int)rnd(POND_R - 6);
    m->x = (int16_t)((GRID_CX + ((COS(a) * d) >> 8)) << 8);
    m->y = (int16_t)((GRID_CY + ((SIN(a) * d) >> 8)) << 8);
    m->heading = (uint8_t)rnd(256);
    m->phase = (uint8_t)rnd(256);
    m->speed = (int16_t)(10 + rnd(14));
}

static void mote_update(mote_t *m)
{
    m->phase = (uint8_t)(m->phase + 3);
    m->heading = (uint8_t)(m->heading + (SIN(m->phase) >> 6));

    int gx = m->x >> 8, gy = m->y >> 8;
    int dx = gx - GRID_CX, dy = gy - GRID_CY;
    int edge = POND_R - 4;
    if (dx * dx + dy * dy > edge * edge)
        m->heading = angle_of(-dx, -dy);

    m->x = (int16_t)(m->x + ((COS(m->heading) * m->speed) >> 8));
    m->y = (int16_t)(m->y + ((SIN(m->heading) * m->speed) >> 8));
}

static void draw_mote(const mote_t *m)
{
    int cx = m->x >> 8, cy = m->y >> 8;
    if (cx < 0 || cx >= GRID_W || cy < 0 || cy >= GRID_H)
        return;

    /* pulse, so the motes breathe rather than sit there */
    int halo = 26 + (SIN(m->phase * 2) >> 4);

    for (int y = cy - 2; y <= cy + 2; y++) {
        if (y < 0 || y >= GRID_H)
            continue;
        int dy = y - cy;
        for (int x = cx - 2; x <= cx + 2; x++) {
            if (x < 0 || x >= GRID_W)
                continue;
            int d2 = (x - cx) * (x - cx) + dy * dy;
            if (d2 > 4 || s_mat[y][x] != MAT_WATER)
                continue;
            light_add(x, y, halo - d2 * 5);
        }
    }

    s_mat[cy][cx] = MAT_MOTE;
    s_light[cy][cx] = (int16_t)(190 + (SIN(m->phase * 2) >> 2));
}
#endif /* MOTE_COUNT > 0 */

// --- Frame ---

static void blit(void)
{
    lv_color_t *dst = s_canvas_buf;
    for (int gy = 0; gy < GRID_H; gy++) {
        const uint8_t *mrow = s_mat[gy];
        const int16_t *lrow = s_light[gy];
        const int8_t *drow = s_dither[gy & 3];
        int i = 0;
        for (int gx = 0; gx < GRID_W; gx++) {
            int l = lrow[gx] + drow[gx & 3];
            int level = (l * LEVELS) >> 8;
            if (level < 0) level = 0;
            if (level >= LEVELS) level = LEVELS - 1;

            lv_color_t c = s_pal[mrow[gx] * LEVELS + level];
            s_row[i++] = c;
            s_row[i++] = c;
            s_row[i++] = c;
            s_row[i++] = c;
        }
        for (int k = 0; k < PIX; k++) {
            memcpy(dst, s_row, DRV_LCD_H_RES * sizeof(lv_color_t));
            dst += DRV_LCD_H_RES;
        }
    }
}

static void pond_step(void)
{
    s_frame++;

    if (--s_next_ambient == 0) {
        /* a koi nosing the surface somewhere */
        const koi_t *k = &s_koi[rnd(KOI_COUNT)];
        ripple_spawn(k->x >> 8, k->y >> 8, 0, RIPPLE_LIFE / 2);
        s_next_ambient = (uint16_t)(90 + rnd(160));
    }

    draw_water();

    for (int i = 0; i < KOI_COUNT; i++) {
        koi_update(&s_koi[i]);
        draw_koi_glow(&s_koi[i]);
    }

    ripples_update();
    ripples_draw();

    for (int i = 0; i < KOI_COUNT; i++)
        draw_koi(&s_koi[i]);

#if PAD_COUNT > 0
    for (int i = 0; i < PAD_COUNT; i++) {
        s_pads[i].phase = (uint8_t)(s_pads[i].phase + 2);
        if ((s_frame & 7) == 0)
            s_pads[i].notch++;
        draw_pad(&s_pads[i]);
    }
#endif

#if MOTE_COUNT > 0
    for (int i = 0; i < MOTE_COUNT; i++) {
        mote_update(&s_motes[i]);
        draw_mote(&s_motes[i]);
    }
#endif

    blit();
    lv_obj_invalidate(s_canvas);
}

static void frame_cb(lv_timer_t *t)
{
    (void)t;
    pond_step();
}

// --- Setup ---

static uint8_t mix(uint8_t a, uint8_t b, int t)
{
    return (uint8_t)((a * (256 - t) + b * t) >> 8);
}

static void palette_init(void)
{
    for (int m = 0; m < MAT_COUNT; m++) {
        for (int l = 0; l < LEVELS; l++) {
            int t = LEVEL_MIX[l];
            uint8_t r = mix(NIGHT_RGB[0], MAT_RGB[m][0], t);
            uint8_t g = mix(NIGHT_RGB[1], MAT_RGB[m][1], t);
            uint8_t b = mix(NIGHT_RGB[2], MAT_RGB[m][2], t);
            if (l == LEVELS - 1) {
                /* brightest step picks up a little of the glow's own colour */
                r = mix(r, GLOW_RGB[0], 36);
                g = mix(g, GLOW_RGB[1], 36);
                b = mix(b, GLOW_RGB[2], 36);
            }
            s_pal[m * LEVELS + l] = lv_color_make(r, g, b);
        }
    }
}

static void dither_init(void)
{
    /* 4x4 ordered dither, scaled to half a light step either side */
    static const uint8_t BAYER[4][4] = {
        {  0,  8,  2, 10 },
        { 12,  4, 14,  6 },
        {  3, 11,  1,  9 },
        { 15,  7, 13,  5 },
    };
    const int step = 256 / LEVELS;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            s_dither[y][x] = (int8_t)((BAYER[y][x] * step) / 16 - step / 2);
}

static void sprite_init(void)
{
    for (int y = 0; y < KOI_H; y++) {
        for (int x = 0; x < KOI_W; x++) {
            switch (KOI_ART[y][x]) {
            case 'm': s_koi_sprite[y][x] = 1; break;
            case 'a': s_koi_sprite[y][x] = 2; break;
            case 'f': s_koi_sprite[y][x] = 3; break;
            default:  s_koi_sprite[y][x] = 0; break;
            }
        }
    }
}

// --- Public API ---

void pond_init(lv_obj_t *parent)
{
    trig_init();
    palette_init();
    dither_init();
    sprite_init();

    for (int i = 0; i < KOI_COUNT; i++)
        koi_reset(&s_koi[i], i);
#if PAD_COUNT > 0
    for (int i = 0; i < PAD_COUNT; i++)
        pad_place(&s_pads[i], i);
#endif
#if MOTE_COUNT > 0
    for (int i = 0; i < MOTE_COUNT; i++)
        mote_reset(&s_motes[i]);
#endif
    s_next_ambient = (uint16_t)(90 + rnd(160));

    size_t buf_size = (size_t)DRV_LCD_H_RES * DRV_LCD_V_RES * sizeof(lv_color_t);
    s_canvas_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!s_canvas_buf) {
        ESP_LOGE(TAG, "Failed to allocate %u byte canvas", (unsigned)buf_size);
        return;
    }

    s_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_canvas, s_canvas_buf, DRV_LCD_H_RES, DRV_LCD_V_RES,
                         LV_IMG_CF_TRUE_COLOR);
    lv_obj_center(s_canvas);

    pond_step();   /* have something on screen before the backlight comes up */
    lv_timer_create(frame_cb, FRAME_MS, NULL);

    ESP_LOGI(TAG, "Pond ready: %dx%d grid, %d koi, %d pads, %d motes",
             GRID_W, GRID_H, KOI_COUNT, PAD_COUNT, MOTE_COUNT);
}

void pond_tap(lv_coord_t x, lv_coord_t y)
{
    int gx = x / PIX, gy = y / PIX;
    if (gx < 0) gx = 0;
    if (gx >= GRID_W) gx = GRID_W - 1;
    if (gy < 0) gy = 0;
    if (gy >= GRID_H) gy = GRID_H - 1;

    /* three staggered rings read as one spreading disturbance */
    ripple_spawn(gx, gy, 0, RIPPLE_LIFE);
    ripple_spawn(gx, gy, 5, RIPPLE_LIFE * 3 / 4);
    ripple_spawn(gx, gy, 11, RIPPLE_LIFE / 2);

    for (int i = 0; i < KOI_COUNT; i++) {
        s_koi[i].tx = (int16_t)gx;
        s_koi[i].ty = (int16_t)gy;
        s_koi[i].boost = (uint16_t)(70 + rnd(30));
    }
}
