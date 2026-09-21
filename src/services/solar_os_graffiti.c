#include "solar_os_graffiti.h"

#include <ctype.h>
#include <string.h>

#define GRAFFITI_FLAG_LETTERS (1U << 0)
#define GRAFFITI_FLAG_NUMBERS (1U << 1)
#define GRAFFITI_FLAG_BOTH (GRAFFITI_FLAG_LETTERS | GRAFFITI_FLAG_NUMBERS)
#define GRAFFITI_RAW_POINT_MAX 12U
#define GRAFFITI_VALUE_SHIFT 0x100
#define GRAFFITI_VALUE_SPACE 0x101
#define GRAFFITI_VALUE_BACKSPACE 0x102
#define GRAFFITI_VALUE_ENTER 0x103
#define GRAFFITI_PI 3.14159265358979323846f

typedef struct {
    int value;
    uint32_t flags;
    uint8_t count;
    solar_os_unistroke_point_t points[GRAFFITI_RAW_POINT_MAX];
} graffiti_raw_template_t;

typedef struct {
    solar_os_unistroke_config_t config;
    solar_os_unistroke_template_t templates[64];
    size_t count;
} graffiti_context_t;

#define P(x_value, y_value) {(float)(x_value), (float)(y_value)}
#define LETTER(ch, ...) {(ch), GRAFFITI_FLAG_LETTERS, \
                         sizeof((solar_os_unistroke_point_t[]){__VA_ARGS__}) / \
                             sizeof(solar_os_unistroke_point_t), \
                         {__VA_ARGS__}}
#define NUMBER(ch, ...) {(ch), GRAFFITI_FLAG_NUMBERS, \
                         sizeof((solar_os_unistroke_point_t[]){__VA_ARGS__}) / \
                             sizeof(solar_os_unistroke_point_t), \
                         {__VA_ARGS__}}
#define CONTROL(value_arg, ...) {(value_arg), GRAFFITI_FLAG_BOTH, \
                                 sizeof((solar_os_unistroke_point_t[]){__VA_ARGS__}) / \
                                     sizeof(solar_os_unistroke_point_t), \
                                 {__VA_ARGS__}}

/*
 * Palm Graffiti 1 canonical stroke order. Coordinates are normalized tracings
 * of the alphabet, numeral, and editing-gesture charts in the PalmPilot
 * Handbook. The first point is always the pen-down point.
 */
static const graffiti_raw_template_t graffiti_raw_templates[] = {
    LETTER('a', P(5, 95), P(50, 5), P(95, 95)),
    LETTER('b', P(15, 5), P(15, 95), P(70, 90), P(90, 70), P(70, 52),
           P(15, 52), P(70, 48), P(90, 25), P(70, 8), P(15, 5)),
    LETTER('c', P(90, 15), P(65, 5), P(25, 15), P(8, 50), P(25, 88), P(90, 90)),
    LETTER('d', P(15, 5), P(15, 95), P(55, 95), P(88, 72), P(88, 30),
           P(55, 5), P(15, 5)),
    LETTER('e', P(90, 10), P(55, 5), P(25, 18), P(42, 45), P(78, 48),
           P(42, 45), P(20, 72), P(45, 94), P(90, 88)),
    LETTER('f', P(88, 8), P(18, 8), P(18, 95)),
    LETTER('g', P(88, 18), P(65, 5), P(25, 12), P(8, 48), P(22, 88),
           P(72, 92), P(90, 65), P(58, 65)),
    LETTER('h', P(15, 5), P(15, 95), P(15, 52), P(50, 35), P(85, 52), P(85, 95)),
    LETTER('i', P(50, 5), P(50, 95)),
    LETTER('j', P(75, 5), P(75, 75), P(58, 95), P(25, 88), P(15, 70)),
    LETTER('k', P(72, 5), P(65, 28), P(18, 48), P(65, 55), P(85, 95)),
    LETTER('l', P(18, 5), P(18, 92), P(90, 92)),
    LETTER('m', P(8, 95), P(8, 35), P(25, 5), P(45, 30), P(45, 95),
           P(45, 35), P(65, 5), P(90, 30), P(90, 95)),
    LETTER('n', P(10, 95), P(10, 8), P(90, 95), P(90, 8)),
    LETTER('o', P(75, 10), P(35, 5), P(8, 35), P(10, 72), P(38, 95),
           P(75, 90), P(92, 55), P(75, 10)),
    LETTER('p', P(18, 5), P(18, 95), P(18, 5), P(65, 5), P(90, 28),
           P(68, 52), P(18, 52)),
    LETTER('q', P(72, 10), P(32, 5), P(8, 38), P(15, 78), P(50, 95),
           P(85, 75), P(88, 35), P(72, 10), P(88, 95)),
    LETTER('r', P(18, 95), P(18, 5), P(65, 5), P(90, 27), P(68, 52),
           P(18, 52), P(88, 95)),
    LETTER('s', P(88, 15), P(65, 5), P(25, 12), P(12, 38), P(75, 62),
           P(88, 82), P(65, 95), P(15, 88)),
    LETTER('t', P(8, 8), P(88, 8), P(88, 95)),
    LETTER('u', P(12, 8), P(12, 70), P(32, 94), P(68, 94), P(88, 70), P(88, 8)),
    LETTER('v', P(8, 8), P(50, 95), P(92, 8)),
    LETTER('w', P(5, 8), P(8, 72), P(25, 95), P(48, 72), P(50, 35),
           P(52, 72), P(75, 95), P(92, 72), P(95, 8)),
    LETTER('x', P(10, 8), P(90, 95), P(50, 52), P(90, 8), P(10, 95)),
    LETTER('y', P(8, 8), P(10, 42), P(28, 58), P(52, 42), P(62, 10),
           P(52, 42), P(40, 95)),
    LETTER('z', P(8, 8), P(92, 8), P(8, 92), P(92, 92)),

    /* Alternate letter forms printed alongside the canonical Palm strokes. */
    LETTER('b', P(18, 95), P(18, 5), P(18, 48), P(70, 48), P(88, 65),
           P(70, 92), P(18, 92), P(18, 48), P(65, 8), P(88, 25), P(70, 48)),
    LETTER('d', P(18, 95), P(18, 5), P(58, 5), P(88, 32), P(88, 68),
           P(58, 95), P(18, 95)),
    LETTER('f', P(18, 95), P(18, 8), P(82, 8)),
    LETTER('g', P(82, 20), P(60, 8), P(25, 15), P(10, 48), P(25, 85),
           P(72, 90), P(85, 65), P(55, 65)),
    LETTER('m', P(8, 55), P(25, 25), P(43, 55), P(43, 95), P(43, 55),
           P(62, 25), P(82, 55), P(82, 95)),
    LETTER('o', P(28, 8), P(68, 5), P(92, 35), P(90, 72), P(62, 95),
           P(25, 90), P(8, 55), P(28, 8)),
    LETTER('p', P(18, 95), P(18, 8), P(65, 8), P(88, 30), P(65, 52), P(18, 52)),
    LETTER('r', P(18, 95), P(18, 8), P(65, 8), P(88, 28), P(65, 50),
           P(18, 50), P(85, 95)),
    LETTER('v', P(92, 8), P(50, 95), P(8, 8)),
    LETTER('x', P(75, 8), P(58, 40), P(18, 50), P(58, 58), P(80, 92)),
    LETTER('y', P(88, 8), P(55, 45), P(28, 20), P(55, 45), P(42, 95)),

    NUMBER('0', P(72, 8), P(30, 5), P(8, 38), P(12, 78), P(45, 96),
           P(82, 82), P(92, 42), P(72, 8)),
    NUMBER('1', P(50, 5), P(50, 95)),
    NUMBER('2', P(10, 25), P(30, 5), P(72, 8), P(90, 30), P(10, 95), P(92, 95)),
    NUMBER('3', P(12, 12), P(68, 5), P(90, 28), P(55, 50), P(90, 70),
           P(68, 95), P(12, 88)),
    NUMBER('4', P(18, 5), P(18, 92), P(88, 92)),
    NUMBER('5', P(88, 8), P(18, 8), P(18, 48), P(70, 48), P(92, 68),
           P(75, 95), P(15, 90)),
    NUMBER('6', P(82, 10), P(48, 5), P(18, 35), P(15, 75), P(42, 95),
           P(78, 88), P(88, 62), P(65, 45), P(18, 55)),
    NUMBER('7', P(8, 8), P(92, 8), P(35, 95)),
    NUMBER('8', P(50, 50), P(22, 30), P(35, 5), P(68, 8), P(80, 32),
           P(50, 50), P(20, 70), P(32, 95), P(68, 92), P(80, 68), P(50, 50)),
    NUMBER('9', P(82, 48), P(42, 52), P(15, 32), P(30, 5), P(68, 8),
           P(88, 38), P(82, 75), P(55, 95), P(18, 92)),

    /* Alternate numeral forms shown in the PalmPilot handbook. */
    NUMBER('0', P(30, 8), P(70, 5), P(92, 38), P(88, 78), P(55, 96),
           P(18, 82), P(8, 42), P(30, 8)),
    NUMBER('5', P(18, 8), P(82, 8), P(18, 8), P(18, 48), P(68, 48),
           P(88, 68), P(70, 94), P(15, 88)),
    NUMBER('8', P(72, 8), P(38, 5), P(18, 28), P(50, 50), P(82, 72),
           P(65, 95), P(30, 90), P(18, 68), P(50, 50), P(82, 28), P(72, 8)),

    CONTROL(GRAFFITI_VALUE_SHIFT, P(50, 95), P(50, 5)),
    CONTROL(GRAFFITI_VALUE_SPACE, P(5, 50), P(95, 50)),
    CONTROL(GRAFFITI_VALUE_BACKSPACE, P(95, 50), P(5, 50)),
    CONTROL(GRAFFITI_VALUE_ENTER, P(92, 8), P(8, 92)),
};

#undef CONTROL
#undef NUMBER
#undef LETTER
#undef P

size_t solar_os_graffiti_context_size(void)
{
    return sizeof(graffiti_context_t);
}

size_t solar_os_graffiti_template_count(void)
{
    return sizeof(graffiti_raw_templates) / sizeof(graffiti_raw_templates[0]);
}

bool solar_os_graffiti_init(void *storage, size_t storage_size)
{
    if (storage == NULL || storage_size < sizeof(graffiti_context_t) ||
        solar_os_graffiti_template_count() >
            sizeof(((graffiti_context_t *)0)->templates) /
                sizeof(solar_os_unistroke_template_t)) {
        return false;
    }
    graffiti_context_t *context = storage;
    memset(context, 0, sizeof(*context));
    context->config = solar_os_unistroke_default_config();
    /* Graffiti assigns meaning to direction, unlike rotation-invariant $1. */
    context->config.rotate_to_indicative_angle = false;
    context->config.angle_range_radians = 8.0f * GRAFFITI_PI / 180.0f;
    context->config.angle_precision_radians = 2.0f * GRAFFITI_PI / 180.0f;
    context->config.minimum_score = 0.62f;
    context->count = solar_os_graffiti_template_count();
    for (size_t i = 0; i < context->count; i++) {
        context->templates[i].value = graffiti_raw_templates[i].value;
        context->templates[i].flags = graffiti_raw_templates[i].flags;
        if (solar_os_unistroke_prepare(graffiti_raw_templates[i].points,
                                      graffiti_raw_templates[i].count,
                                      &context->config,
                                      &context->templates[i].path) !=
            SOLAR_OS_UNISTROKE_OK) {
            memset(context, 0, sizeof(*context));
            return false;
        }
    }
    return true;
}

solar_os_graffiti_zone_t solar_os_graffiti_zone_for_start(int16_t x,
                                                           uint16_t width)
{
    if (x >= 0 && width > 0U && (uint32_t)x * 3U >= (uint32_t)width * 2U) {
        return SOLAR_OS_GRAFFITI_NUMBERS;
    }
    return SOLAR_OS_GRAFFITI_LETTERS;
}

bool solar_os_graffiti_recognize(
    const void *storage,
    solar_os_graffiti_zone_t zone,
    const solar_os_unistroke_point_t *points,
    size_t point_count,
    solar_os_graffiti_result_t *result)
{
    if (storage == NULL || result == NULL) {
        return false;
    }
    memset(result, 0, sizeof(*result));
    const graffiti_context_t *context = storage;
    const uint32_t flags = zone == SOLAR_OS_GRAFFITI_NUMBERS ?
        GRAFFITI_FLAG_NUMBERS : GRAFFITI_FLAG_LETTERS;
    solar_os_unistroke_result_t match;
    if (solar_os_unistroke_recognize(points, point_count,
                                     context->templates, context->count,
                                     flags, &context->config, &match) !=
            SOLAR_OS_UNISTROKE_OK ||
        !match.matched) {
        return true;
    }
    result->matched = true;
    result->score = match.score;
    if (match.value >= 0 && match.value <= 0xff) {
        result->action = SOLAR_OS_GRAFFITI_ACTION_CHARACTER;
        result->character = (char)match.value;
    } else if (match.value == GRAFFITI_VALUE_SHIFT) {
        result->action = SOLAR_OS_GRAFFITI_ACTION_SHIFT;
    } else if (match.value == GRAFFITI_VALUE_SPACE) {
        result->action = SOLAR_OS_GRAFFITI_ACTION_SPACE;
    } else if (match.value == GRAFFITI_VALUE_BACKSPACE) {
        result->action = SOLAR_OS_GRAFFITI_ACTION_BACKSPACE;
    } else if (match.value == GRAFFITI_VALUE_ENTER) {
        result->action = SOLAR_OS_GRAFFITI_ACTION_ENTER;
    } else {
        result->matched = false;
    }
    return true;
}

bool solar_os_graffiti_apply(solar_os_graffiti_case_state_t *state,
                             const solar_os_graffiti_result_t *result,
                             char *character)
{
    if (state == NULL || result == NULL || character == NULL || !result->matched) {
        return false;
    }
    if (result->action == SOLAR_OS_GRAFFITI_ACTION_SHIFT) {
        if (state->caps_lock) {
            state->caps_lock = false;
            state->shift_pending = false;
        } else if (state->shift_pending) {
            state->caps_lock = true;
            state->shift_pending = false;
        } else {
            state->shift_pending = true;
        }
        return false;
    }
    if (result->action == SOLAR_OS_GRAFFITI_ACTION_CHARACTER) {
        *character = result->character;
        if (isalpha((unsigned char)*character) &&
            (state->shift_pending || state->caps_lock)) {
            *character = (char)toupper((unsigned char)*character);
        }
        state->shift_pending = false;
        return true;
    }
    state->shift_pending = false;
    if (result->action == SOLAR_OS_GRAFFITI_ACTION_SPACE) {
        *character = ' ';
        return true;
    }
    if (result->action == SOLAR_OS_GRAFFITI_ACTION_BACKSPACE) {
        *character = '\b';
        return true;
    }
    if (result->action == SOLAR_OS_GRAFFITI_ACTION_ENTER) {
        *character = '\r';
        return true;
    }
    return false;
}
