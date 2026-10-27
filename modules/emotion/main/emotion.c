#include "solar_os_emotion_abi.h"
#include <string.h>

/* The poses are authored geometry based on the supplied Cozmo expression
 * reference. Coordinates use a 1000 x 600 face canvas, with y downwards. */
typedef struct {
    float x, y, w, h, radius, top, bottom, tilt, lower_curve;
} eye_t;
typedef struct { eye_t eye[2]; } pose_t;
#define E(x,y,w,h,r,t,b,a,c) {x,y,w,h,r,t,b,a,c}
#define PAIR(y,w,h,r,t,b,a,c) {{E(-160,y,w,h,r,t,b,a,c), E(160,y,w,h,r,t,b,-a,c)}}
static const pose_t poses[] = {
    PAIR(0,250,270,65,0,0,0,0),                    /* neutral */
    PAIR(-70,330,16,8,0,0,0,0),                   /* blink high */
    PAIR(0,250,260,100,0,.64f,0,0),               /* happy */
    PAIR(-12,250,260,120,0,.67f,0,.12f),          /* glee */
    PAIR(70,330,16,8,0,0,0,0),                    /* blink low */
    PAIR(80,250,170,60,.28f,0,-.23f,0),           /* sad down */
    PAIR(-95,250,230,65,.12f,0,-.24f,0),          /* sad up */
    PAIR(0,250,240,80,.06f,0,-.12f,0),            /* worried */
    PAIR(0,250,210,30,0,.48f,0,0),                /* focused */
    {{E(-160,-15,250,160,55,.65f,.06f,0,0),
      E(160,-30,250,135,40,.65f,.06f,0,0)}},       /* annoyed */
    PAIR(0,310,340,130,0,0,0,0),                  /* surprised */
    {{E(-160,-30,250,265,60,0,0,0,0),
      E(160,30,250,210,70,.22f,0,-.12f,0)}},       /* skeptic */
    PAIR(55,250,175,60,.58f,0,0,0),               /* bored */
    {{E(-160,55,250,175,60,.62f,0,0,0),
      E(160,45,250,195,60,.57f,0,0,0)}},          /* unimpressed */
    {{E(-160,30,250,150,35,.08f,0,-.05f,0),
      E(160,55,250,40,20,0,0,.04f,0)}},          /* sleepy */
    {{E(-160,-15,250,180,40,0,.35f,0,0),
      E(160,0,250,155,30,0,.3f,0,0)}},           /* suspicious */
    PAIR(0,270,85,35,0,.2f,0,0),                  /* squint */
    PAIR(35,250,210,65,.38f,0,.18f,0),            /* angry */
    PAIR(0,260,260,35,.18f,0,.30f,0),             /* furious */
    PAIR(-10,250,300,85,0,.16f,0,0),              /* scared */
    PAIR(0,280,340,140,0,0,0,0),                  /* awe */
};
static const char *const names[] = {
    "neutral", "blink_high", "happy", "glee", "blink_low", "sad_down",
    "sad_up", "worried", "focused", "annoyed", "surprised", "skeptic",
    "bored", "unimpressed", "sleepy", "suspicious", "squint", "angry",
    "furious", "scared", "awe"
};
typedef struct {
    pose_t from, to;
    uint64_t started, reaction_end, next_blink, blink_started;
    uint32_t duration, persistent, random;
    int32_t gaze_x, gaze_y;
    int blinking, reacting;
} emotion_t;

static float mix(float a, float b, float t) { return a + (b - a) * t; }
static uint32_t random_next(emotion_t *s)
{
    uint32_t x = s->random;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return s->random = x;
}
static pose_t evaluate(emotion_t *s, uint64_t now)
{
    float t = now <= s->started ? 0 :
        (!s->duration || now - s->started >= s->duration ? 1 :
         (float)(now - s->started) / s->duration);
    if (!s->duration) t = 1;
    t = t * t * (3 - 2 * t);
    pose_t p;
    for (unsigned i = 0; i < 2; ++i) {
#define MIX(member) p.eye[i].member = mix(s->from.eye[i].member, s->to.eye[i].member, t)
        MIX(x); MIX(y); MIX(w); MIX(h); MIX(radius); MIX(top); MIX(bottom);
        MIX(tilt); MIX(lower_curve);
#undef MIX
    }
    return p;
}
static void advance(emotion_t *s, uint64_t now)
{
    if (s->reacting && now >= s->reaction_end) {
        s->from = evaluate(s, s->reaction_end);
        s->to = poses[s->persistent];
        s->started = s->reaction_end;
        s->duration = 200;
        s->reacting = 0;
    }
}
static const char *pose_name(uint32_t pose)
{ return pose < SOLAR_OS_EMOTION_POSE_COUNT ? names[pose] : NULL; }
static int pose_find(const char *name)
{
    if (name) for (unsigned i = 0; i < SOLAR_OS_EMOTION_POSE_COUNT; ++i)
        if (strlen(name) == strlen(names[i]) && !memcmp(name, names[i], strlen(name))) return (int)i;
    return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static int init(void *state, uint64_t now, uint32_t seed)
{
    if (!state) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    emotion_t *s = state;
    memset(s, 0, sizeof(*s));
    s->from = s->to = poses[0]; s->started = now;
    s->random = seed ? seed : 1;
    s->next_blink = now + 2500 + random_next(s) % 2500;
    return 0;
}
static int change(void *state, uint32_t pose, uint32_t duration, uint64_t now)
{
    if (!state || pose >= SOLAR_OS_EMOTION_POSE_COUNT || duration > 60000)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    emotion_t *s = state;
    advance(s, now);
    s->from = evaluate(s, now); s->to = poses[pose];
    s->started = now; s->duration = duration;
    return 0;
}
static int set(void *state, uint32_t pose, uint32_t duration, uint64_t now)
{
    int result = change(state, pose, duration, now);
    if (!result) {
        emotion_t *s = state;
        s->persistent = pose; s->reacting = 0;
    }
    return result;
}
static int react(void *state, uint32_t pose, uint32_t hold, uint32_t duration, uint64_t now)
{
    if (hold > 60000) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    int result = change(state, pose, duration, now);
    if (!result) {
        emotion_t *s = state;
        s->reaction_end = now + duration + hold; s->reacting = 1;
    }
    return result;
}
static int look(void *state, int32_t x, int32_t y)
{
    if (!state || x < -1000 || x > 1000 || y < -1000 || y > 1000)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    emotion_t *s = state; s->gaze_x = x; s->gaze_y = y;
    return 0;
}
static int blink(void *state, uint64_t now)
{
    if (!state) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    emotion_t *s = state; s->blinking = 1; s->blink_started = now;
    return 0;
}
static float absolute(float x) { return x < 0 ? -x : x; }
static void draw_eye(const eye_t *e, float scale, float cx, float cy,
                     float blink_scale, uint8_t *pixels, unsigned w, unsigned h, unsigned stride)
{
    float ew = e->w * scale, eh = e->h * scale;
    float radius = e->radius * scale;
    if (radius > ew / 2) radius = ew / 2;
    if (radius > eh / 2) radius = eh / 2;
    float eye_x = cx + e->x * scale, eye_y = cy + e->y * scale;
    int left = (int)(eye_x - ew / 2), right = (int)(eye_x + ew / 2 + 1);
    int top = (int)(eye_y - eh / 2), bottom = (int)(eye_y + eh / 2 + 1);
    if (left < 0) left = 0;
    if (top < 0) top = 0;
    if (right > (int)w) right = w;
    if (bottom > (int)h) bottom = h;
    for (int y = top; y < bottom; ++y) for (int x = left; x < right; ++x) {
        float dx = x + .5f - eye_x, dy = y + .5f - eye_y;
        float u = dx / ew;
        float upper = eh * (-.5f + e->top + e->tilt * u);
        float lower = eh * (.5f - e->bottom - e->lower_curve * (1 - 4 * u * u));
        float mid = (upper + lower) / 2;
        float half = (lower - upper) * blink_scale / 2;
        if (half < .65f) half = .65f;
        if (dy < mid - half || dy > mid + half) continue;
        float qx = absolute(dx) - (ew / 2 - radius);
        float qy = absolute(dy) - (eh / 2 - radius);
        if (absolute(dx) > ew / 2 || absolute(dy) > eh / 2 ||
            (qx > 0 && qy > 0 && qx * qx + qy * qy > radius * radius)) continue;
        pixels[(size_t)y * stride + ((unsigned)x >> 2)] |= 3U << ((x & 3) * 2);
    }
}
static int render(void *state, uint64_t now, uint8_t *pixels, size_t bytes,
                  uint16_t w, uint16_t h, uint16_t stride, int animate)
{
    if (!state || !pixels || !w || !h || w > 2048 || h > 2048 ||
        stride < (w + 3U) / 4U || bytes < (size_t)stride * h)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    emotion_t *s = state; advance(s, now);
    pose_t p = evaluate(s, now);
    float openness = 1;
    if (animate && !s->blinking && now >= s->next_blink) blink(s, now);
    if (s->blinking) {
        uint64_t elapsed = now >= s->blink_started ? now - s->blink_started : 0;
        if (elapsed >= 180) {
            s->blinking = 0; s->next_blink = now + 2500 + random_next(s) % 3500;
        } else {
            openness = elapsed < 70 ? 1 - (float)elapsed / 70 : (float)(elapsed - 70) / 110;
        }
    }
    memset(pixels, 0, (size_t)stride * h);
    float scale = (float)w / 1000;
    if (scale > (float)h / 600) scale = (float)h / 600;
    float idle = 0;
    if (animate) {
        float t = (float)(now % 6000) / 3000;
        idle = (t < 1 ? t : 2 - t) * 5 - 2.5f;
    }
    float cx = w / 2.0f + s->gaze_x * .11f * scale;
    float cy = h / 2.0f + (s->gaze_y * .075f + idle) * scale;
    for (unsigned i = 0; i < 2; ++i)
        draw_eye(&p.eye[i], scale, cx, cy, openness, pixels, w, h, stride);
    return 0;
}
static uint32_t cue_duration(uint32_t cue)
{
    static const uint32_t durations[] = {0, 240, 480, 420, 320};
    return cue < SOLAR_OS_EMOTION_CUE_COUNT ? durations[cue] : 0;
}
static int sound(uint32_t cue, uint32_t rate, uint32_t offset, int16_t *out, size_t count)
{
    if (cue >= SOLAR_OS_EMOTION_CUE_COUNT || rate < 8000 || rate > 96000 ||
        !out || count > 4096 || (uint64_t)offset + count > UINT32_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    float duration = cue_duration(cue) / 1000.0f;
    for (size_t i = 0; i < count; ++i) {
        float t = (float)(offset + i) / rate;
        if (t >= duration || cue == SOLAR_OS_EMOTION_SILENT) { out[i] = 0; continue; }
        float start = cue == SOLAR_OS_EMOTION_SIGH ? 650 : 700;
        float slope = cue == SOLAR_OS_EMOTION_SIGH ? -850 : 2600;
        float phase = start * t + .5f * slope * t * t;
        float frac = phase - (uint32_t)phase;
        float wave = frac < .5f ? 4 * frac - 1 : 3 - 4 * frac;
        float envelope = t < .015f ? t / .015f :
            (duration - t < .04f ? (duration - t) / .04f : 1);
        if (cue == SOLAR_OS_EMOTION_GIGGLE || cue == SOLAR_OS_EMOTION_ALERT) {
            unsigned pulse = (unsigned)(t * (cue == SOLAR_OS_EMOTION_GIGGLE ? 12 : 8));
            if (pulse & 1U) envelope = 0;
        }
        out[i] = (int16_t)(wave * envelope * 7000);
    }
    return 0;
}
const solar_os_emotion_api_v1_t *solar_os_emotion_engine_api_v1(void)
{
    static const solar_os_emotion_api_v1_t api = {
        .abi_version = SOLAR_OS_EMOTION_ABI, .struct_size = sizeof(api),
        .state_bytes = sizeof(emotion_t), .pose_name = pose_name, .pose_find = pose_find,
        .init = init, .set = set, .react = react, .look = look, .blink = blink,
        .render = render, .cue_duration_ms = cue_duration, .sound = sound,
    };
    return &api;
}
