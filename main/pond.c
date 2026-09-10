/**
 * A small pixel-art koi pond, lit like a cave.
 *
 * The pond is a fixed WORLD_W x WORLD_W world of "world pixels". The screen
 * is a camera window into it: every world pixel is drawn as a s_pix x s_pix
 * block, so the zoom level decides both how much of the pond is visible and
 * how chunky it looks. Fully zoomed out the whole pond fits the panel;
 * zoomed in the camera drifts after one of the koi.
 *
 * Two buffers are kept per visible cell: a material (water, pad, koi, ...)
 * and a light level. Colour only comes together at blit time, where each
 * material is looked up in its own dark-to-lit ramp. Anything that glows
 * just adds light, so the koi, the ripples and the motes all sit in the
 * same lighting model.
 *
 * Per frame: ambient water light -> koi glow -> ripple light -> koi bodies
 * -> lily pads -> motes -> blit. Glow is only added to open water so a ring
 * never washes out a fish or a pad.
 *
 * How many koi, pads and motes exist is a runtime number, not a compile
 * time one. They are free to roam past the rim, and everything fades out
 * with distance from the middle of the pond, so the count you can see
 * drifts either side of the count being simulated.
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
#include "sound.h"

static const char *TAG = "pond";

// --- Geometry ---
//
// World coordinates are in world pixels. At the widest zoom one world pixel
// covers PIX_MIN screen pixels and the world exactly fills the panel, which
// fixes the size of every buffer below.

#define PIX_MIN   4
#define WORLD_W   (DRV_LCD_H_RES / PIX_MIN)   /* 103 */
#define WORLD_CX  (WORLD_W / 2)
#define WORLD_CY  (WORLD_W / 2)
#define POND_R    (WORLD_W / 2)               /* the display is round */
#define FRAME_MS  40

/* Everything fades out with distance, reaching black a little past the rim.
 * Roaming limits sit inside that, so wanderers dim rather than blink out. */
#define FADE_INNER (POND_R * 3 / 5)   /* lit normally out to here ... */
#define FADE_R     (POND_R + 12)      /* ... then down to nothing by here */
#define KOI_ROAM  (POND_R + 4)
#define PAD_ROAM  (POND_R + 3)
#define MOTE_ROAM (POND_R + 2)

/* Zoom detents, in screen pixels per world pixel. */
static const uint8_t ZOOM_PIX[] = { 4, 5, 6, 8, 10, 13 };
#define ZOOM_STEPS ((int)(sizeof ZOOM_PIX / sizeof ZOOM_PIX[0]))

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
#define KOI_REACH 8    /* body bounding box half-size, world pixels */
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
    return n ? esp_random() % n : 0;
}

/// How lit a thing at (wx, wy) should be, 256 in the middle of the pond down
/// to 0 out past the rim. Everything that has its own colour is scaled by
/// this, so wanderers fade into the dark instead of floating in the void.
static int rim_fade(int wx, int wy)
{
    int dx = wx - WORLD_CX, dy = wy - WORLD_CY;
    int d = isqrt32((int32_t)dx * dx + (int32_t)dy * dy);
    if (d <= FADE_INNER)
        return 256;                    /* full brightness across the middle */
    int f = 256 - (256 * (d - FADE_INNER)) / (FADE_R - FADE_INNER);
    return f < 0 ? 0 : f;
}

// --- Entities, all positioned in world coordinates, 8.8 fixed point ---

typedef struct {
    int32_t x, y;
    uint8_t heading;   /* 0..255 */
    uint8_t phase;     /* tail wiggle phase */
    int16_t speed;     /* 8.8 world pixels per frame */
    int8_t  turn;
    uint8_t type;
    uint16_t boost;    /* frames left chasing the last tap */
    int16_t tx, ty;    /* what the koi is swimming towards */
} koi_t;

typedef struct {
    int32_t x, y;
    uint8_t r;
    uint8_t notch;     /* direction of the wedge cut out of the pad */
    uint8_t phase;     /* bob phase */
    uint8_t heading;   /* which way it is drifting */
    int16_t speed;
    bool flower;
} pad_t;

typedef struct {
    int32_t x, y;
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

#define RIPPLE_SPEED 340   /* 8.8 world pixels per frame */
#define RIPPLE_LIFE  46

static koi_t s_koi[POND_MAX_KOI];
static pad_t s_pads[POND_MAX_PADS];
static mote_t s_motes[POND_MAX_MOTES];
static ripple_t s_ripples[MAX_RIPPLES];

/// How much is simulated right now. Runtime, not baked in.
static int s_koi_count;
static int s_pad_count;
static int s_mote_count;

static uint8_t s_mat[WORLD_W][WORLD_W];
static int16_t s_light[WORLD_W][WORLD_W];
static int8_t s_dither[4][4];

static lv_color_t s_row[DRV_LCD_H_RES];
static lv_color_t *s_canvas_buf;
static lv_obj_t *s_canvas;
static uint32_t s_frame;
static uint16_t s_next_surface;
static uint16_t s_next_distant;

// --- Camera ---

static int s_zoom;                  /* index into ZOOM_PIX */
static int s_pix = PIX_MIN;         /* screen pixels per world pixel */
static int s_gw = WORLD_W;          /* visible cells across */
static int s_gh = WORLD_W;
static int32_t s_cam_x, s_cam_y;    /* 8.8 world coords of the view centre */
static int s_ox, s_oy;              /* world coords of the top-left cell */
static int s_focus = -1;            /* koi the camera is following, or -1 */

#define CAM_LEAD 5                  /* world pixels ahead of the focus koi */

/// Light stays in 0..255; blit quantises it into LEVELS steps.
#define LIGHT_MAX 255

static inline void light_add(int vx, int vy, int amount)
{
    int v = s_light[vy][vx] + amount;
    if (v < 0) v = 0;
    if (v > LIGHT_MAX) v = LIGHT_MAX;
    s_light[vy][vx] = (int16_t)v;
}

static void camera_apply_zoom(void)
{
    s_pix = ZOOM_PIX[s_zoom];
    s_gw = (DRV_LCD_H_RES + s_pix - 1) / s_pix;
    s_gh = (DRV_LCD_V_RES + s_pix - 1) / s_pix;
}

static void camera_update(void)
{
    int32_t tx = (int32_t)WORLD_CX << 8;
    int32_t ty = (int32_t)WORLD_CY << 8;

    if (s_focus >= 0 && s_focus < s_koi_count) {
        /* aim a little ahead of the fish: the easing below lags behind, and
         * the two roughly cancel out to keep it framed */
        const koi_t *k = &s_koi[s_focus];
        tx = k->x + COS(k->heading) * CAM_LEAD;
        ty = k->y + SIN(k->heading) * CAM_LEAD;
    }

    /* keep the window from wandering far past the rim into dead water */
    int off_max = POND_R + 8 - s_gw / 2;
    if (off_max < 0)
        off_max = 0;
    int dx = (int)((tx >> 8) - WORLD_CX);
    int dy = (int)((ty >> 8) - WORLD_CY);
    int d2 = dx * dx + dy * dy;
    if (d2 > off_max * off_max) {
        int d = isqrt32(d2);
        if (d > 0) {
            tx = ((int32_t)(WORLD_CX + dx * off_max / d)) << 8;
            ty = ((int32_t)(WORLD_CY + dy * off_max / d)) << 8;
        }
    }

    /* a lazy camera operator: always drifting towards the target */
    s_cam_x += (tx - s_cam_x) >> 4;
    s_cam_y += (ty - s_cam_y) >> 4;

    s_ox = (int)(s_cam_x >> 8) - s_gw / 2;
    s_oy = (int)(s_cam_y >> 8) - s_gh / 2;
}

// --- Water ---

#define AMBIENT_CORE 120   /* light on open water at the centre of the pond */

static void draw_water(void)
{
    int a1 = (int)s_frame;
    int a2 = -2 * (int)s_frame;
    int a3 = 3 * (int)s_frame;
    const int r2max = POND_R * POND_R;

    for (int vy = 0; vy < s_gh; vy++) {
        int y = s_oy + vy;
        int dy = y - WORLD_CY;
        int dy2 = dy * dy;
        uint8_t *mrow = s_mat[vy];
        int16_t *lrow = s_light[vy];
        for (int vx = 0; vx < s_gw; vx++) {
            int x = s_ox + vx;
            int dx = x - WORLD_CX;
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

            mrow[vx] = MAT_WATER;
            lrow[vx] = (int16_t)l;
        }
    }
}

// --- Ripples ---

static void ripple_spawn(int wx, int wy, int delay, int life)
{
    for (int i = 0; i < MAX_RIPPLES; i++) {
        if (s_ripples[i].active)
            continue;
        s_ripples[i].x = (int16_t)wx;
        s_ripples[i].y = (int16_t)wy;
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

        int cx = rp->x - s_ox, cy = rp->y - s_oy;
        int lo = rad - 4;
        if (lo < 0)
            lo = 0;
        int hi = rad + 4;
        int lo2 = lo * lo, hi2 = hi * hi, r2 = rad * rad;
        int den = 2 * rad;

        int y0 = cy - hi, y1 = cy + hi;
        if (y0 < 0) y0 = 0;
        if (y1 >= s_gh) y1 = s_gh - 1;

        for (int vy = y0; vy <= y1; vy++) {
            int dy = vy - cy;
            int dy2 = dy * dy;
            if (dy2 > hi2)
                continue;
            int half = isqrt32(hi2 - dy2);
            int x0 = cx - half, x1 = cx + half;
            if (x0 < 0) x0 = 0;
            if (x1 >= s_gw) x1 = s_gw - 1;

            const uint8_t *mrow = s_mat[vy];
            for (int vx = x0; vx <= x1; vx++) {
                if (mrow[vx] != MAT_WATER)
                    continue;
                int dx = vx - cx;
                int d2 = dx * dx + dy2;
                if (d2 < lo2)
                    continue;

                /* (d2 - r2) / 2r approximates the signed distance to the ring */
                int k = (d2 - r2) / den;
                if (k >= -1 && k <= 1)
                    light_add(vx, vy, str * 34);
                else if (k == -2 || k == 2)
                    light_add(vx, vy, str * 12);
                else if (k > 2 && str >= 2)
                    light_add(vx, vy, -22);
            }
        }
    }
}

// --- Lily pads ---

static void pad_place(pad_t *p, int placed)
{
    p->r = (uint8_t)(6 + rnd(4));
    p->notch = (uint8_t)rnd(256);
    p->phase = (uint8_t)rnd(256);
    p->heading = (uint8_t)rnd(256);
    p->speed = (int16_t)(3 + rnd(5));      /* a very lazy drift */
    p->flower = (p->r >= 7) && (rnd(3) == 0);

    for (int tries = 0; tries < 80; tries++) {
        int reach = POND_R - p->r - 3;
        int a = (int)rnd(256);
        /* pick the radius by area so the pads do not bunch up in the middle */
        int d = isqrt32((int32_t)rnd((uint32_t)(reach * reach) + 1));
        int cx = WORLD_CX + ((COS(a) * d) >> 8);
        int cy = WORLD_CY + ((SIN(a) * d) >> 8);
        p->x = (int32_t)cx << 8;
        p->y = (int32_t)cy << 8;

        bool clear = true;
        for (int j = 0; j < placed; j++) {
            int dx = cx - (int)(s_pads[j].x >> 8);
            int dy = cy - (int)(s_pads[j].y >> 8);
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

/// Pads float, so they drift, nudge each other apart and turn back when they
/// reach the far bank.
static void pad_update(pad_t *p, int idx)
{
    p->phase = (uint8_t)(p->phase + 2);
    if ((s_frame & 7) == 0)
        p->notch++;

    if (rnd(120) == 0)
        p->heading = (uint8_t)(p->heading + (int)rnd(41) - 20);

    /* Left to itself a drifting leaf is a 2D random walk, and a random walk
     * spends most of its time far from where it started — over a few minutes
     * every pad ends up stranded against the rim. So the further out a pad
     * gets, the harder the pond turns it back: free in the middle, firmly
     * steered at the edge. */
    int wx = (int)(p->x >> 8), wy = (int)(p->y >> 8);
    int dx = wx - WORLD_CX, dy = wy - WORLD_CY;
    const int home = POND_R * 2 / 3;
    const int home2 = home * home;
    int d2 = dx * dx + dy * dy;
    if (d2 > home2) {
        int pull = (64 * (d2 - home2)) / (PAD_ROAM * PAD_ROAM - home2);
        if (pull > 64)
            pull = 64;
        int inward = angle_of(-dx, -dy);
        int diff = (int8_t)((uint8_t)inward - p->heading);
        int step = pull / 24 + 1;                  /* 1..3 units per frame */
        if (diff > step)  diff = step;
        if (diff < -step) diff = -step;
        p->heading = (uint8_t)(p->heading + diff);
    }

    /* leaves crowd but do not stack: push gently off any close neighbour */
    for (int j = 0; j < s_pad_count; j++) {
        if (j == idx)
            continue;
        int ox = wx - (int)(s_pads[j].x >> 8);
        int oy = wy - (int)(s_pads[j].y >> 8);
        int min = p->r + s_pads[j].r + 2;
        int d2 = ox * ox + oy * oy;
        if (d2 == 0 || d2 >= min * min)
            continue;
        int d = isqrt32(d2);
        if (d == 0)
            d = 1;
        p->x += (ox * 20) / d;
        p->y += (oy * 20) / d;
    }

    p->x += (COS(p->heading) * p->speed) >> 8;
    p->y += (SIN(p->heading) * p->speed) >> 8;
}

/// Pads read as dark silhouettes with a lit rim on the side facing the light.
static void draw_pad(const pad_t *p)
{
    int wx = (int)(p->x >> 8), wy = (int)(p->y >> 8);
    int fade = rim_fade(wx, wy);
    int cx = wx - s_ox;
    int cy = wy - s_oy + (SIN(p->phase) >> 8);   /* gentle bob, +/- 1 px */
    int r = p->r;
    int r2 = r * r;
    int inner2 = (r - 1) * (r - 1);
    int ncs = COS(p->notch), nsn = SIN(p->notch);
    int rim_lift = (90 * fade) >> 8;

    for (int dy = -r; dy <= r; dy++) {
        int vy = cy + dy;
        if (vy < 0 || vy >= s_gh)
            continue;
        for (int dx = -r; dx <= r; dx++) {
            int vx = cx + dx;
            if (vx < 0 || vx >= s_gw)
                continue;
            int d2 = dx * dx + dy * dy;
            if (d2 > r2)
                continue;

            /* narrow wedge notch, cut in from the rim but not to the centre */
            int dot = (dx * ncs + dy * nsn) >> 8;
            int cross = (-dx * nsn + dy * ncs) >> 8;
            if (dot > r / 4 && abs(cross) * 5 < (dot - r / 4) * 2)
                continue;

            int lit = s_light[vy][vx];
            if (d2 >= inner2 && dx + dy < 0) {
                s_mat[vy][vx] = MAT_PAD_RIM;         /* catches the light */
                lit = lit + rim_lift;
            } else {
                s_mat[vy][vx] = MAT_PAD;             /* in its own shadow */
                lit = (lit * 3) / 4 - 10 - (dx + dy) * 2;
            }
            if (lit < 0) lit = 0;
            if (lit > LIGHT_MAX) lit = LIGHT_MAX;
            s_light[vy][vx] = (int16_t)lit;
        }
    }

    if (!p->flower)
        return;

    /* a bright heart with four dim petals on the diagonals */
    static const int8_t PETALS[4][2] = { { -1, -1 }, { 1, -1 }, { -1, 1 }, { 1, 1 } };
    for (int i = 0; i < 4; i++) {
        int vx = cx + PETALS[i][0], vy = cy + PETALS[i][1];
        if (vx < 0 || vx >= s_gw || vy < 0 || vy >= s_gh)
            continue;
        s_mat[vy][vx] = MAT_FLOWER;
        s_light[vy][vx] = (int16_t)((165 * fade) >> 8);
    }
    if (cx >= 0 && cx < s_gw && cy >= 0 && cy < s_gh) {
        s_mat[cy][cx] = MAT_FLOWER;
        s_light[cy][cx] = (int16_t)((LIGHT_MAX * fade) >> 8);
    }
}

// --- Koi ---

static void koi_reset(koi_t *k, int i)
{
    int a = (int)rnd(256);
    int d = (int)rnd(POND_R - 12);
    k->x = (int32_t)(WORLD_CX + ((COS(a) * d) >> 8)) << 8;
    k->y = (int32_t)(WORLD_CY + ((SIN(a) * d) >> 8)) << 8;
    k->heading = (uint8_t)rnd(256);
    k->phase = (uint8_t)rnd(256);
    k->speed = (int16_t)(70 + rnd(40));
    k->turn = 0;
    k->type = (uint8_t)(i % 3);
    k->boost = 0;
}

static void koi_update(koi_t *k)
{
    int wx = (int)(k->x >> 8), wy = (int)(k->y >> 8);
    int dx = wx - WORLD_CX, dy = wy - WORLD_CY;
    int desired = -1;

    /* they are allowed out past the rim, they just turn back eventually */
    if (dx * dx + dy * dy > KOI_ROAM * KOI_ROAM) {
        desired = angle_of(-dx, -dy);
    } else if (k->boost) {
        int tdx = k->tx - wx, tdy = k->ty - wy;
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
    int wx = (int)(k->x >> 8), wy = (int)(k->y >> 8);
    int amount = (48 * rim_fade(wx, wy)) >> 8;
    if (amount <= 0)
        return;

    int cx = wx - s_ox, cy = wy - s_oy;
    const int r = KOI_GLOW_R;
    const int r2 = r * r;

    for (int vy = cy - r; vy <= cy + r; vy++) {
        if (vy < 0 || vy >= s_gh)
            continue;
        int dy = vy - cy, dy2 = dy * dy;
        if (dy2 > r2)
            continue;
        const uint8_t *mrow = s_mat[vy];
        for (int vx = cx - r; vx <= cx + r; vx++) {
            if (vx < 0 || vx >= s_gw)
                continue;
            if (mrow[vx] != MAT_WATER)
                continue;
            int dx = vx - cx;
            int d2 = dx * dx + dy2;
            if (d2 > r2)
                continue;
            light_add(vx, vy, (amount * (r2 - d2)) / r2);
        }
    }
}

/// A koi under a lily pad still shows as light bleeding through the leaf,
/// so the fish never vanishes completely when you are zoomed in on it.
static void draw_koi_underglow(const koi_t *k)
{
    int wx = (int)(k->x >> 8), wy = (int)(k->y >> 8);
    int amount = (72 * rim_fade(wx, wy)) >> 8;
    if (amount <= 0)
        return;

    int cx = wx - s_ox, cy = wy - s_oy;
    const int r = 7;
    const int r2 = r * r;

    for (int vy = cy - r; vy <= cy + r; vy++) {
        if (vy < 0 || vy >= s_gh)
            continue;
        int dy = vy - cy, dy2 = dy * dy;
        if (dy2 > r2)
            continue;
        const uint8_t *mrow = s_mat[vy];
        for (int vx = cx - r; vx <= cx + r; vx++) {
            if (vx < 0 || vx >= s_gw)
                continue;
            if (mrow[vx] != MAT_PAD)
                continue;
            int dx = vx - cx;
            int d2 = dx * dx + dy2;
            if (d2 > r2)
                continue;
            light_add(vx, vy, (amount * (r2 - d2)) / r2);
        }
    }
}

static void draw_koi(const koi_t *k)
{
    int wx = (int)(k->x >> 8), wy = (int)(k->y >> 8);
    int fade = rim_fade(wx, wy);
    int body_lit = 30 + ((200 * fade) >> 8);
    int tail_lit = 20 + ((128 * fade) >> 8);

    int cx = wx - s_ox, cy = wy - s_oy;
    int cs = COS(k->heading), sn = SIN(k->heading);
    const uint8_t *mats = KOI_MAT[k->type];
    int wig_amp = SIN(k->phase);

    for (int vy = cy - KOI_REACH; vy <= cy + KOI_REACH; vy++) {
        if (vy < 0 || vy >= s_gh)
            continue;
        int dy = vy - cy;
        for (int vx = cx - KOI_REACH; vx <= cx + KOI_REACH; vx++) {
            if (vx < 0 || vx >= s_gw)
                continue;
            int dx = vx - cx;

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

            s_mat[vy][vx] = mats[v - 1];
            /* the body is what glows; the tail is thinner and half sunk */
            s_light[vy][vx] = (int16_t)((v == 3) ? tail_lit : body_lit);
        }
    }
}

/// Index of the koi nearest the camera, for the zoom to latch onto.
static int koi_nearest_camera(void)
{
    int best = 0;
    int32_t best_d2 = INT32_MAX;
    for (int i = 0; i < s_koi_count; i++) {
        int32_t dx = (s_koi[i].x - s_cam_x) >> 8;
        int32_t dy = (s_koi[i].y - s_cam_y) >> 8;
        int32_t d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best = i;
        }
    }
    return best;
}

// --- Drifting motes ---

static void mote_reset(mote_t *m)
{
    int a = (int)rnd(256);
    int d = (int)rnd(POND_R - 6);
    m->x = (int32_t)(WORLD_CX + ((COS(a) * d) >> 8)) << 8;
    m->y = (int32_t)(WORLD_CY + ((SIN(a) * d) >> 8)) << 8;
    m->heading = (uint8_t)rnd(256);
    m->phase = (uint8_t)rnd(256);
    m->speed = (int16_t)(10 + rnd(14));
}

static void mote_update(mote_t *m)
{
    m->phase = (uint8_t)(m->phase + 3);
    m->heading = (uint8_t)(m->heading + (SIN(m->phase) >> 6));

    int wx = (int)(m->x >> 8), wy = (int)(m->y >> 8);
    int dx = wx - WORLD_CX, dy = wy - WORLD_CY;
    if (dx * dx + dy * dy > MOTE_ROAM * MOTE_ROAM)
        m->heading = angle_of(-dx, -dy);

    m->x += (COS(m->heading) * m->speed) >> 8;
    m->y += (SIN(m->heading) * m->speed) >> 8;
}

static void draw_mote(const mote_t *m)
{
    int wx = (int)(m->x >> 8), wy = (int)(m->y >> 8);
    int fade = rim_fade(wx, wy);
    int cx = wx - s_ox, cy = wy - s_oy;
    if (cx < 0 || cx >= s_gw || cy < 0 || cy >= s_gh || fade <= 0)
        return;

    /* pulse, so the motes breathe rather than sit there */
    int halo = ((26 + (SIN(m->phase * 2) >> 4)) * fade) >> 8;

    for (int vy = cy - 2; vy <= cy + 2; vy++) {
        if (vy < 0 || vy >= s_gh)
            continue;
        int dy = vy - cy;
        for (int vx = cx - 2; vx <= cx + 2; vx++) {
            if (vx < 0 || vx >= s_gw)
                continue;
            int d2 = (vx - cx) * (vx - cx) + dy * dy;
            if (d2 > 4 || s_mat[vy][vx] != MAT_WATER)
                continue;
            light_add(vx, vy, halo - d2 * 5);
        }
    }

    s_mat[cy][cx] = MAT_MOTE;
    s_light[cy][cx] = (int16_t)((((190 + (SIN(m->phase * 2) >> 2))) * fade) >> 8);
}

// --- Frame ---

/// Expand the visible cells into the canvas, s_pix screen pixels each. The
/// zoom steps do not all divide the panel evenly, so the last block of a row
/// or column is clipped — it falls outside the round bezel anyway.
static void blit(void)
{
    const int pix = s_pix;
    lv_color_t *dst = s_canvas_buf;
    int rows_left = DRV_LCD_V_RES;

    for (int vy = 0; vy < s_gh && rows_left > 0; vy++) {
        const uint8_t *mrow = s_mat[vy];
        const int16_t *lrow = s_light[vy];
        const int8_t *drow = s_dither[vy & 3];

        int i = 0;
        for (int vx = 0; vx < s_gw && i < DRV_LCD_H_RES; vx++) {
            int l = lrow[vx] + drow[vx & 3];
            int level = (l * LEVELS) >> 8;
            if (level < 0) level = 0;
            if (level >= LEVELS) level = LEVELS - 1;

            lv_color_t c = s_pal[mrow[vx] * LEVELS + level];
            int n = DRV_LCD_H_RES - i;
            if (n > pix)
                n = pix;
            while (n--)
                s_row[i++] = c;
        }

        int n = rows_left < pix ? rows_left : pix;
        rows_left -= n;
        while (n--) {
            memcpy(dst, s_row, DRV_LCD_H_RES * sizeof(lv_color_t));
            dst += DRV_LCD_H_RES;
        }
    }
}

static void pond_step(void)
{
    s_frame++;

    if (s_koi_count > 0 && --s_next_surface == 0) {
        /* a koi nosing the surface somewhere */
        const koi_t *k = &s_koi[rnd((uint32_t)s_koi_count)];
        ripple_spawn((int)(k->x >> 8), (int)(k->y >> 8), 0, RIPPLE_LIFE / 2);
        sound_play(SOUND_SURFACE);
        s_next_surface = (uint16_t)(90 + rnd(160));
    }

    if (--s_next_distant == 0) {
        /* something falling in over by the far bank */
        int a = (int)rnd(256);
        int d = POND_R - 6 - (int)rnd(8);
        ripple_spawn(WORLD_CX + ((COS(a) * d) >> 8),
                     WORLD_CY + ((SIN(a) * d) >> 8), 0, RIPPLE_LIFE * 2 / 3);
        sound_play(SOUND_DISTANT);
        s_next_distant = (uint16_t)(220 + rnd(420));
    }

    camera_update();
    draw_water();

    for (int i = 0; i < s_koi_count; i++) {
        koi_update(&s_koi[i]);
        draw_koi_glow(&s_koi[i]);
    }

    ripples_update();
    ripples_draw();

    for (int i = 0; i < s_koi_count; i++)
        draw_koi(&s_koi[i]);

    for (int i = 0; i < s_pad_count; i++) {
        pad_update(&s_pads[i], i);
        draw_pad(&s_pads[i]);
    }
    for (int i = 0; i < s_koi_count; i++)
        draw_koi_underglow(&s_koi[i]);

    for (int i = 0; i < s_mote_count; i++) {
        mote_update(&s_motes[i]);
        draw_mote(&s_motes[i]);
    }

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

static int clamp_count(int want, int max)
{
    if (want < 0) return 0;
    if (want > max) return max;
    return want;
}

// --- Public API ---

void pond_set_population(int koi, int pads, int motes)
{
    koi = clamp_count(koi, POND_MAX_KOI);
    pads = clamp_count(pads, POND_MAX_PADS);
    motes = clamp_count(motes, POND_MAX_MOTES);

    for (int i = s_koi_count; i < koi; i++)
        koi_reset(&s_koi[i], i);
    for (int i = s_pad_count; i < pads; i++)
        pad_place(&s_pads[i], i);
    for (int i = s_mote_count; i < motes; i++)
        mote_reset(&s_motes[i]);

    s_koi_count = koi;
    s_pad_count = pads;
    s_mote_count = motes;

    if (s_focus >= s_koi_count)
        s_focus = (s_zoom > 0 && s_koi_count > 0) ? koi_nearest_camera() : -1;
}

void pond_get_population(int *koi, int *pads, int *motes)
{
    if (koi)   *koi = s_koi_count;
    if (pads)  *pads = s_pad_count;
    if (motes) *motes = s_mote_count;
}

void pond_init(lv_obj_t *parent)
{
    trig_init();
    palette_init();
    dither_init();
    sprite_init();

    s_cam_x = (int32_t)WORLD_CX << 8;
    s_cam_y = (int32_t)WORLD_CY << 8;
    camera_apply_zoom();

    pond_set_population(CONFIG_MOCHI_POND_KOI_COUNT,
                        CONFIG_MOCHI_POND_LILY_COUNT,
                        CONFIG_MOCHI_POND_MOTE_COUNT);

    s_next_surface = (uint16_t)(90 + rnd(160));
    s_next_distant = (uint16_t)(220 + rnd(420));

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

    ESP_LOGI(TAG, "Pond ready: %dx%d world, %d koi, %d pads, %d motes",
             WORLD_W, WORLD_W, s_koi_count, s_pad_count, s_mote_count);
}

void pond_tap(lv_coord_t x, lv_coord_t y)
{
    int wx = s_ox + x / s_pix;
    int wy = s_oy + y / s_pix;

    /* three staggered rings read as one spreading disturbance */
    ripple_spawn(wx, wy, 0, RIPPLE_LIFE);
    ripple_spawn(wx, wy, 5, RIPPLE_LIFE * 3 / 4);
    ripple_spawn(wx, wy, 11, RIPPLE_LIFE / 2);

    for (int i = 0; i < s_koi_count; i++) {
        s_koi[i].tx = (int16_t)wx;
        s_koi[i].ty = (int16_t)wy;
        s_koi[i].boost = (uint16_t)(70 + rnd(30));
    }
}

bool pond_zoom(int delta)
{
    int z = s_zoom + delta;
    if (z < 0) z = 0;
    if (z >= ZOOM_STEPS) z = ZOOM_STEPS - 1;
    if (z == s_zoom)
        return false;

    s_zoom = z;
    camera_apply_zoom();

    /* wide open the camera sits on the pond; any closer and it picks a koi
     * to drift after, so zooming in never lands on empty water */
    if (s_zoom == 0 || s_koi_count == 0)
        s_focus = -1;
    else if (s_focus < 0)
        s_focus = koi_nearest_camera();
    return true;
}
