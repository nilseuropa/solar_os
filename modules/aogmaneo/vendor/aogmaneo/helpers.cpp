// ----------------------------------------------------------------------------
//  AOgmaNeo
//  Copyright(c) 2020-2025 Ogma Intelligent Systems Corp. All rights reserved.
//
//  This copy of AOgmaNeo is licensed to you under the terms described
//  in the AOGMANEO_LICENSE.md file included in this distribution.
// ----------------------------------------------------------------------------

#include "helpers.h"

#ifdef USE_OMP
#include <omp.h>
#endif

using namespace aon;

float aon::expf(
    float x
) {
#ifdef USE_STD_MATH
    return std::exp(x);
#else
    if (x > 0.0f) {
        float p = x;
        int f = 1;

        float res = 1.0f + x;

        for (int n = 2; n <= exp_iters; n++) {
            p *= x;
            f *= n;

            res += p / f;
        }

        return res;
    }

    float p = -x;
    int f = 1;

    float res = 1.0f - x;

    for (int n = 2; n <= exp_iters; n++) {
        p *= -x;
        f *= n;

        res += p / f;
    }

    return 1.0f / res;
#endif
}

float aon::log2f(
    float x
) {
#ifdef USE_STD_MATH
    return std::log2(x);
#else
    if (x <= 0.0f)
        return limit_min;

    // get exponent
    union {
        float x;
        unsigned int i;
    } u;

    u.x = x;

    const int bias = 127;

    int exponent = ((u.i >> 23) & 0xff) - bias;

    float y = x;
    float res = exponent;

    if (exponent > 0)
        y /= (1 << exponent);
    else if (exponent < 0)
        y *= (1 << -exponent);

    int m = 0;

    for (int n = 0; n < log_iters; n++) {
        float z = y;

        if (z == 1.0f)
            break;

        while (z < 2.0f) {
            z *= z;
            m++;
        }

        // if have full precision
        if (m >= 32)
            break;

        res += 1.0f / (1 << m);

        y = z * 0.5f;
    }

    return res;
#endif
}

float aon::logf(
    float x
) {
#ifdef USE_STD_MATH
    return std::log(x);
#else
    return log2f(x) * log2_e_inv;
#endif
}

float aon::sqrtf(
    float x
) {
#ifdef USE_STD_MATH
    return std::sqrt(x);
#else
    // quake method
    union {
        float x;
        int i;
    } u;

    u.x = x;
    u.i = 0x5f3759df - (u.i >> 1);

    return x * u.x * (1.5f - 0.5f * x * u.x * u.x);
#endif
}

float aon::powf(
    float x,
    float y
) {
#ifdef USE_STD_MATH
    return std::pow(x, y);
#else
    return expf(y * logf(x));
#endif
}

float aon::sinf(
    float x
) {
#ifdef USE_STD_MATH
    return std::sin(x);
#else
    x = modf(x, pi2);

    if (x < -pi)
        x += pi2;
    else if (x > pi)
        x -= pi2;

    float p = x;
    int f = 1;

    float res = x;

    for (int n = 1; n <= trig_iters; n++) {
        p *= -x * x;

        int f1 = n * 2;

        f *= f1 * (f1 + 1);

        res += p / f;
    }

    return res;
#endif
}

float aon::cosf(
    float x
) {
#ifdef USE_STD_MATH
    return std::cos(x);
#else
    return sinf(pi_over_2 - x);
#endif
}

#ifdef USE_OMP
void aon::set_num_threads(
    int num_threads
) {
    omp_set_num_threads(num_threads);
}

int aon::get_num_threads() {
    return omp_get_num_threads();
}
#else
void aon::set_num_threads(
    int num_threads
) {}

int aon::get_num_threads() {
    return 0;
}
#endif

Int2 aon::min_overhang(
    const Int2 &pos,
    const Int2 &size,
    const Int2 &radii
) {
    Int2 new_pos = pos;

    bool overhang_px = (new_pos.x + radii.x >= size.x);
    bool overhang_nx = (new_pos.x - radii.x < 0);
    bool overhang_py = (new_pos.y + radii.y >= size.y);
    bool overhang_ny = (new_pos.y - radii.y < 0);

    if (overhang_px && !overhang_nx)
       new_pos.x = size.x - 1 - radii.x;
    else if (overhang_nx && !overhang_px)
       new_pos.x = radii.x;

    if (overhang_py && !overhang_ny)
       new_pos.y = size.y - 1 - radii.y;
    else if (overhang_ny && !overhang_py)
       new_pos.y = radii.y;

    return new_pos;
}

uint64_t aon::global_state = rand_get_state(12345);

float aon::rand_normalf(
    uint64_t* state
) {
    float u1 = randf(state);
    float u2 = randf(state);

    return sqrtf(-2.0f * logf(u1)) * cosf(pi2 * u2);
}
