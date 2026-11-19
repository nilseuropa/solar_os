// ----------------------------------------------------------------------------
//  AOgmaNeo
//  Copyright(c) 2020-2025 Ogma Intelligent Systems Corp. All rights reserved.
//
//  This copy of AOgmaNeo is licensed to you under the terms described
//  in the AOGMANEO_LICENSE.md file included in this distribution.
// ----------------------------------------------------------------------------

#pragma once

#include "array.h"
#include <stdint.h>

#ifdef USE_STD_MATH
#include <cmath>
#include <algorithm>
#endif

#define PARALLEL_FOR _Pragma("omp parallel for")
#define ATOMIC _Pragma("omp atomic")

namespace aon {
const int exp_iters = 6;
const int log_iters = 6;
const int trig_iters = 6;
const float pi = 3.14159f;
const float pi2 = pi * 2.0f;
const float pi_over_2 = pi * 0.5f;
const float log2_e = 1.44269f;
const float log2_e_inv = 1.0f / log2_e;
const float limit_min = -999999.0f;
const float limit_max = 999999.0f;
const float limit_small = 0.000001f;

const int rand_subseed_offset = 12345;
const int init_weight_noisei = 8;
const float init_weight_noisef = 0.01f;

typedef unsigned char Byte;
typedef signed char S_Byte;

inline float modf(
    float x,
    float y
) {
    return x - static_cast<int>(x / y) * y;
}

inline int ceil_divide(
    int x,
    int y
) {
    return (x + y - 1) / y;
}

float expf(
    float x
);

float log2f(
    float x
);

float logf(
    float x
);

float sqrtf(
    float x
);

float powf(
    float x,
    float y
);

float sinf(
    float x
);

float cosf(
    float x
);

inline int ceilf(
    float x
) {
    if (x > 0.0f)
        return (x - static_cast<int>(x)) > 0.0f ? static_cast<int>(x + 1) : static_cast<int>(x);

    return (x - static_cast<int>(x)) < 0.0f ? static_cast<int>(x - 1) : static_cast<int>(x);
}

inline int roundf2i(
    float x
) {
    return static_cast<int>(x + (x > 0.0f) - 0.5f);
}

inline Byte roundf2b(
    float x
) {
    return static_cast<Byte>(x + 0.5f);
}

inline S_Byte roundf2sb(
    float x
) {
    return static_cast<S_Byte>(x + (x > 0.0f) - 0.5f);
}

template <typename T>
T min(
    T left,
    T right
) {
    return (left < right) * (left - right) + right;
}

template <typename T>
T max(
    T left,
    T right
) {
    return (left > right) * (left - right) + right;
}

template <typename T>
T abs(
    T x
) {
#ifdef USE_STD_MATH
    return std::abs(x);
#else
    if (x >= 0)
        return x;

    return  -x;
#endif
}

template <typename T>
void swap(
    T &left,
    T &right
) {
    T temp = left;
    left = right;
    right = temp;
}

// open_mp stuff, does nothing if USE_OMP is not set
void set_num_threads(
    int num_threads
);

int get_num_threads();

// Vector types
template <typename T> 
struct Vec2 {
    T x, y;

    Vec2() {}

    Vec2(
        T x,
        T y
    )
    : x(x), y(y)
    {}
};

template <typename T> 
struct Vec3 {
    T x, y, z;
    T pad;

    Vec3()
    {}

    Vec3(
        T x,
        T y,
        T z
    )
    : x(x), y(y), z(z)
    {}
};

template <typename T> 
struct Vec4 {
    T x, y, z, w;

    Vec4()
    {}

    Vec4(
        T x,
        T y,
        T z,
        T w
    )
    : x(x), y(y), z(z), w(w)
    {}
};

// some basic definitions
typedef Vec2<int> Int2;
typedef Vec3<int> Int3;
typedef Vec4<int> Int4;
typedef Vec2<float> Float2;
typedef Vec3<float> Float3;
typedef Vec4<float> Float4;

typedef Array<Byte> Byte_Buffer;
typedef Array<S_Byte> S_Byte_Buffer;
typedef Array<unsigned short> U_Short_Buffer;
typedef Array<short> Short_Buffer;
typedef Array<unsigned int> U_Int_Buffer;
typedef Array<int> Int_Buffer;
typedef Array<float> Float_Buffer;

typedef Array_View<Byte> Byte_Buffer_View;
typedef Array_View<S_Byte> S_Byte_Buffer_View;
typedef Array_View<unsigned short> U_Short_Buffer_View;
typedef Array_View<short> Short_Buffer_View;
typedef Array_View<unsigned int> U_Int_Buffer_View;
typedef Array_View<int> Int_Buffer_View;
typedef Array_View<float> Float_Buffer_View;

// --- circular buffer ---

template <typename T> 
struct Circle_Buffer {
    Array<T> data;
    int start;

    Circle_Buffer()
    :
    start(0)
    {}

    void resize(
        int size
    ) {
        data.resize(size);
    }

    void push_front() {
        start--;

        if (start < 0)
            start += data.size();
    }

    T &front() {
        return data[start];
    }

    const T &front() const {
        return data[start];
    }

    T &back() {
        return data[(start + data.size() - 1) % data.size()];
    }

    const T &back() const {
        return data[(start + data.size() - 1) % data.size()];
    }

    T &operator[](
        int index
    ) {
        return data[(start + index) % data.size()];
    }

    const T &operator[](
        int index
    ) const {
        return data[(start + index) % data.size()];
    }

    int size() const {
        return data.size();
    }
};

// --- bounds ---

// bounds check from (0, 0) to upper_bound
inline bool in_bounds0(
    const Int2 &pos,
    const Int2 &upper_bound
) {
    return pos.x >= 0 && pos.x < upper_bound.x && pos.y >= 0 && pos.y < upper_bound.y;
}

// bounds check in range
inline bool in_bounds(
    const Int2 &pos,
    const Int2 &lower_bound,
    const Int2 &upper_bound
) {
    return pos.x >= lower_bound.x && pos.x < upper_bound.x && pos.y >= lower_bound.y && pos.y < upper_bound.y;
}

// --- projections ---

inline Int2 project(
    const Int2 &pos, // position
    const Float2 &to_scalars // ratio of sizes
) {
    return Int2((pos.x + 0.5f) * to_scalars.x, (pos.y + 0.5f) * to_scalars.y);
}

inline Int2 projectf(
    const Float2 &pos, // position
    const Float2 &to_scalars // ratio of sizes
) {
    return Int2((pos.x + 0.5f) * to_scalars.x, (pos.y + 0.5f) * to_scalars.y);
}

Int2 min_overhang(
    const Int2 &pos,
    const Int2 &size,
    const Int2 &radii
);

inline Int2 min_overhang(
    const Int2 &pos,
    const Int2 &size,
    int radius
) {
    return min_overhang(pos, size, Int2(radius, radius));
}

// --- addressing ---

// row-major
inline int address2(
    const Int2 &pos, // position
    const Int2 &dims // dimensions to ravel with
) {
    return pos.y + pos.x * dims.y;
}

inline int address3(
    const Int3 &pos, // position
    const Int3 &dims // dimensions to ravel with
) {
    return pos.z + dims.z * (pos.y + dims.y * pos.x);
}

inline int address4(
    const Int4 &pos, // position
    const Int4 &dims // dimensions to ravel with
) {
    return pos.w + dims.w * (pos.z + dims.z * (pos.y + dims.y * pos.x));
}

// --- noninearities ---

inline float sigmoidf(
    float x
) {
#ifdef USE_STD_MATH
    return std::tanh(x * 0.5f) * 0.5f + 0.5f;
#else
    if (x < 0.0f) {
        float z = expf(x);

        return z / (1.0f + z);
    }
    
    return 1.0f / (1.0f + expf(-x));
#endif
}

inline float logitf(
    float x
) {
#ifdef USE_STD_MATH
    return std::log(x / (1.0f - x));
#else
    return logf(x / (1.0f - x));
#endif
}

inline float tanhf(
    float x
) {
#ifdef USE_STD_MATH
    return std::tanh(x);
#else
    if (x < 0.0f) {
        float z = expf(2.0f * x);

        return (z - 1.0f) / (z + 1.0f);
    }

    float z = expf(-2.0f * x);

    return -(z - 1.0f) / (z + 1.0f);
#endif
}

inline float symlogf(
    float x
) {
#ifdef USE_STD_MATH
    return ((x > 0.0f) * 2.0f - 1.0f) * std::log(std::abs(x) + 1.0f);
#else
    return ((x > 0.0f) * 2.0f - 1.0f) * logf(abs(x) + 1.0f);
#endif
}

inline float symexpf(
    float x
) {
#ifdef USE_STD_MATH
    return ((x > 0.0f) * 2.0f - 1.0f) * (std::exp(std::abs(x)) - 1.0f);
#else
    return ((x > 0.0f) * 2.0f - 1.0f) * (expf(abs(x)) - 1.0f);
#endif
}

const float softplus_limit = 4.0f;

inline float softplusf(
    float x
) {
    float in_range = (x < softplus_limit);

    return logf(1.0f + expf(x * in_range)) * in_range + x * (1.0f - in_range);
}

// --- rng ---

// PCG32 https://en.wikipedia.org/wiki/Permuted_congruential_generator
extern uint64_t global_state;

const uint64_t pcg_multiplier = UINT64_C(6364136223846793005);
const uint64_t pcg_increment = UINT64_C(1442695040888963407);

const unsigned int rand_max = 0x00ffffff;

inline unsigned int rotr32(unsigned int x, unsigned int r) {
    return x >> (r & 31) | x << (-r & 31);
}

inline uint64_t rand_get_state(
    uint64_t seed
) {
    uint64_t state = seed + pcg_increment;

    state = state * pcg_multiplier + pcg_increment;

    return state;
}

inline unsigned int rand(
    uint64_t* state = &global_state
) {
    uint64_t x = *state;
    unsigned int count = static_cast<unsigned int>(x >> 59);

    *state = x * pcg_multiplier + pcg_increment;

    x ^= x >> 18;

    return rotr32(static_cast<unsigned int>(x >> 27), count);
}

inline float randf(
    uint64_t* state = &global_state
) {
    return static_cast<float>(rand(state) % rand_max) / static_cast<float>(rand_max);
}

inline float randf(
    float low,
    float high,
    uint64_t* state = &global_state
) {
    return low + (high - low) * randf(state);
}

float rand_normalf(
    uint64_t* state = &global_state
);

inline int rand_roundf(
    float x,
    uint64_t* state = &global_state
) {
    int i = static_cast<int>(x);
    float abs_rem = abs(x - i);

    int s = (x > 0.0f) * 2 - 1;

    return i + (randf(state) < abs_rem) * s;
}

// --- serialization ---

class Stream_Writer {
public:
    virtual ~Stream_Writer() {}

    virtual void write(
        const void* data,
        long len
    ) = 0;
};

class Stream_Reader {
public:
    virtual ~Stream_Reader() {}

    virtual void read(
        void* data,
        long len
    ) = 0;
};
}
