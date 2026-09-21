/*
 * Portable implementation of the $1 Unistroke Recognizer described by
 * Wobbrock, Wilson and Li (UIST 2007). See solar_os_unistroke.LICENSE.
 */
#include "solar_os_unistroke.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define UNISTROKE_PI 3.14159265358979323846f
#define UNISTROKE_PHI 0.5f * (-1.0f + 2.23606797749978969641f)
#define UNISTROKE_EPSILON 0.0001f

static float point_distance(solar_os_unistroke_point_t a,
                            solar_os_unistroke_point_t b)
{
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return sqrtf(dx * dx + dy * dy);
}

static float path_length(const solar_os_unistroke_point_t *points, size_t count)
{
    float length = 0.0f;
    for (size_t i = 1; i < count; i++) {
        length += point_distance(points[i - 1U], points[i]);
    }
    return length;
}

static void resample(const solar_os_unistroke_point_t *input,
                     size_t input_count,
                     float length,
                     solar_os_unistroke_path_t *output)
{
    const float interval = length / (float)(SOLAR_OS_UNISTROKE_POINT_COUNT - 1U);
    size_t segment = 1U;
    float segment_start_distance = 0.0f;
    float next_distance = interval;
    solar_os_unistroke_point_t previous = input[0];
    output->points[0] = previous;

    for (size_t out = 1U; out + 1U < SOLAR_OS_UNISTROKE_POINT_COUNT; out++) {
        while (segment < input_count) {
            const solar_os_unistroke_point_t current = input[segment];
            const float distance = point_distance(previous, current);
            if (segment_start_distance + distance >= next_distance &&
                distance > UNISTROKE_EPSILON) {
                const float ratio =
                    (next_distance - segment_start_distance) / distance;
                previous.x += ratio * (current.x - previous.x);
                previous.y += ratio * (current.y - previous.y);
                output->points[out] = previous;
                segment_start_distance = next_distance;
                next_distance += interval;
                break;
            }
            segment_start_distance += distance;
            previous = current;
            segment++;
        }
        if (segment >= input_count) {
            output->points[out] = input[input_count - 1U];
        }
    }
    output->points[SOLAR_OS_UNISTROKE_POINT_COUNT - 1U] = input[input_count - 1U];
}

static solar_os_unistroke_point_t centroid(const solar_os_unistroke_path_t *path)
{
    solar_os_unistroke_point_t center = {0.0f, 0.0f};
    for (size_t i = 0; i < SOLAR_OS_UNISTROKE_POINT_COUNT; i++) {
        center.x += path->points[i].x;
        center.y += path->points[i].y;
    }
    center.x /= (float)SOLAR_OS_UNISTROKE_POINT_COUNT;
    center.y /= (float)SOLAR_OS_UNISTROKE_POINT_COUNT;
    return center;
}

static void rotate_by(solar_os_unistroke_path_t *path, float angle)
{
    const solar_os_unistroke_point_t center = centroid(path);
    const float cosine = cosf(angle);
    const float sine = sinf(angle);
    for (size_t i = 0; i < SOLAR_OS_UNISTROKE_POINT_COUNT; i++) {
        const float x = path->points[i].x - center.x;
        const float y = path->points[i].y - center.y;
        path->points[i].x = x * cosine - y * sine + center.x;
        path->points[i].y = x * sine + y * cosine + center.y;
    }
}

static void scale_to_square(solar_os_unistroke_path_t *path, float size)
{
    float min_x = FLT_MAX;
    float min_y = FLT_MAX;
    float max_x = -FLT_MAX;
    float max_y = -FLT_MAX;
    for (size_t i = 0; i < SOLAR_OS_UNISTROKE_POINT_COUNT; i++) {
        min_x = fminf(min_x, path->points[i].x);
        min_y = fminf(min_y, path->points[i].y);
        max_x = fmaxf(max_x, path->points[i].x);
        max_y = fmaxf(max_y, path->points[i].y);
    }
    const float width = max_x - min_x;
    const float height = max_y - min_y;
    const bool vertical_line = height > UNISTROKE_EPSILON &&
        width < height * 0.05f;
    const bool horizontal_line = width > UNISTROKE_EPSILON &&
        height < width * 0.05f;
    const float scale_x = width > UNISTROKE_EPSILON && !vertical_line ?
        size / width : 0.0f;
    const float scale_y = height > UNISTROKE_EPSILON && !horizontal_line ?
        size / height : 0.0f;
    for (size_t i = 0; i < SOLAR_OS_UNISTROKE_POINT_COUNT; i++) {
        path->points[i].x = width > UNISTROKE_EPSILON && !vertical_line ?
            (path->points[i].x - min_x) * scale_x : size * 0.5f;
        path->points[i].y = height > UNISTROKE_EPSILON && !horizontal_line ?
            (path->points[i].y - min_y) * scale_y : size * 0.5f;
    }
}

static void translate_to_origin(solar_os_unistroke_path_t *path)
{
    const solar_os_unistroke_point_t center = centroid(path);
    for (size_t i = 0; i < SOLAR_OS_UNISTROKE_POINT_COUNT; i++) {
        path->points[i].x -= center.x;
        path->points[i].y -= center.y;
    }
}

static float path_distance(const solar_os_unistroke_path_t *a,
                           const solar_os_unistroke_path_t *b)
{
    float distance = 0.0f;
    for (size_t i = 0; i < SOLAR_OS_UNISTROKE_POINT_COUNT; i++) {
        distance += point_distance(a->points[i], b->points[i]);
    }
    return distance / (float)SOLAR_OS_UNISTROKE_POINT_COUNT;
}

static float distance_at_angle(const solar_os_unistroke_path_t *candidate,
                               const solar_os_unistroke_path_t *reference,
                               float angle)
{
    solar_os_unistroke_path_t rotated = *candidate;
    rotate_by(&rotated, angle);
    return path_distance(&rotated, reference);
}

static float distance_at_best_angle(const solar_os_unistroke_path_t *candidate,
                                    const solar_os_unistroke_path_t *reference,
                                    float range,
                                    float precision)
{
    if (range <= 0.0f || precision <= 0.0f) {
        return path_distance(candidate, reference);
    }
    float lower = -range;
    float upper = range;
    float x1 = UNISTROKE_PHI * lower + (1.0f - UNISTROKE_PHI) * upper;
    float x2 = (1.0f - UNISTROKE_PHI) * lower + UNISTROKE_PHI * upper;
    float f1 = distance_at_angle(candidate, reference, x1);
    float f2 = distance_at_angle(candidate, reference, x2);
    while (fabsf(upper - lower) > precision) {
        if (f1 < f2) {
            upper = x2;
            x2 = x1;
            f2 = f1;
            x1 = UNISTROKE_PHI * lower + (1.0f - UNISTROKE_PHI) * upper;
            f1 = distance_at_angle(candidate, reference, x1);
        } else {
            lower = x1;
            x1 = x2;
            f1 = f2;
            x2 = (1.0f - UNISTROKE_PHI) * lower + UNISTROKE_PHI * upper;
            f2 = distance_at_angle(candidate, reference, x2);
        }
    }
    return fminf(f1, f2);
}

solar_os_unistroke_config_t solar_os_unistroke_default_config(void)
{
    return (solar_os_unistroke_config_t) {
        .rotate_to_indicative_angle = true,
        .square_size = 250.0f,
        .angle_range_radians = 45.0f * UNISTROKE_PI / 180.0f,
        .angle_precision_radians = 2.0f * UNISTROKE_PI / 180.0f,
        .minimum_score = 0.70f,
    };
}

solar_os_unistroke_status_t solar_os_unistroke_prepare(
    const solar_os_unistroke_point_t *points,
    size_t point_count,
    const solar_os_unistroke_config_t *config,
    solar_os_unistroke_path_t *path)
{
    if (points == NULL || point_count < 2U || config == NULL || path == NULL ||
        config->square_size <= 0.0f) {
        return SOLAR_OS_UNISTROKE_INVALID_ARGUMENT;
    }
    const float length = path_length(points, point_count);
    if (length <= UNISTROKE_EPSILON) {
        return SOLAR_OS_UNISTROKE_TOO_SHORT;
    }
    resample(points, point_count, length, path);
    if (config->rotate_to_indicative_angle) {
        const solar_os_unistroke_point_t center = centroid(path);
        const float angle = atan2f(center.y - path->points[0].y,
                                   center.x - path->points[0].x);
        rotate_by(path, -angle);
    }
    scale_to_square(path, config->square_size);
    translate_to_origin(path);
    return SOLAR_OS_UNISTROKE_OK;
}

solar_os_unistroke_status_t solar_os_unistroke_recognize(
    const solar_os_unistroke_point_t *points,
    size_t point_count,
    const solar_os_unistroke_template_t *templates,
    size_t template_count,
    uint32_t allowed_flags,
    const solar_os_unistroke_config_t *config,
    solar_os_unistroke_result_t *result)
{
    if (templates == NULL || template_count == 0U || result == NULL) {
        return SOLAR_OS_UNISTROKE_INVALID_ARGUMENT;
    }
    memset(result, 0, sizeof(*result));
    solar_os_unistroke_path_t candidate;
    const solar_os_unistroke_status_t status =
        solar_os_unistroke_prepare(points, point_count, config, &candidate);
    if (status != SOLAR_OS_UNISTROKE_OK) {
        return status;
    }
    float best = FLT_MAX;
    size_t best_index = 0U;
    bool found = false;
    for (size_t i = 0; i < template_count; i++) {
        if (allowed_flags != 0U && (templates[i].flags & allowed_flags) == 0U) {
            continue;
        }
        const float distance = distance_at_best_angle(
            &candidate, &templates[i].path,
            config->angle_range_radians, config->angle_precision_radians);
        if (distance < best) {
            best = distance;
            best_index = i;
            found = true;
        }
    }
    if (!found) {
        return SOLAR_OS_UNISTROKE_INVALID_ARGUMENT;
    }
    const float half_diagonal = 0.5f * sqrtf(2.0f * config->square_size *
                                             config->square_size);
    const float score = fmaxf(0.0f, 1.0f - best / half_diagonal);
    result->template_index = best_index;
    result->value = templates[best_index].value;
    result->score = score;
    result->distance = best;
    result->matched = score >= config->minimum_score;
    return SOLAR_OS_UNISTROKE_OK;
}
