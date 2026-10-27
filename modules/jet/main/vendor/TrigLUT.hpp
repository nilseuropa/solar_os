#ifndef TRIGLUT_HPP
#define TRIGLUT_HPP

#include <cstdint>
#include "FastMath.hpp"
#include "JetConfig.hpp"

namespace Renderer
{
    const int ANGLE_MAX = 360;
    const int FIXED_POINT_SCALE = 1 << 10; // Adjusted for better precision
    constexpr int32_t FIXED_POINT_SHIFT = 10; // Use a fixed-point scale factor of 2^10
#define M_PI 3.14159265358979323846

    extern int32_t sin_table[ANGLE_MAX];
    extern int32_t tan_table[ANGLE_MAX];

    void initializeTrigTables();

    // Integer lookup functions
    inline int32_t lookupSinI(int angle) {
        int idx = angle % ANGLE_MAX;
        if (idx < 0) idx += ANGLE_MAX;
        return sin_table[idx];
    }

    inline int32_t lookupCosI(int angle) {
        int idx = (angle + 90) % ANGLE_MAX;
        if (idx < 0) idx += ANGLE_MAX;
        return sin_table[idx];
    }

    inline int32_t lookupTanI(int angle) {
        int idx = angle % ANGLE_MAX;
        if (idx < 0) idx += ANGLE_MAX;
        return tan_table[idx];
    }

    #if FLOAT_CAMERA_ANGLES
    // Number of subdivisions based on respective scales
    const int FLOAT_SIN_SUBDIVISIONS = FLOAT_SIN_CACHE_SCALE;
    const int FLOAT_TAN_SUBDIVISIONS = FLOAT_TAN_CACHE_SCALE;
    const int FLOAT_SIN_ANGLE_MAX = 360 * FLOAT_SIN_SUBDIVISIONS;
    const int FLOAT_TAN_ANGLE_MAX = 360 * FLOAT_TAN_SUBDIVISIONS;

    extern float float_sin_table[FLOAT_SIN_ANGLE_MAX];
    #if FLOAT_TAN_CACHE_SCALE > 1
    extern float float_tan_table[FLOAT_TAN_ANGLE_MAX];
    #endif

    // Helper functions for float angle lookups
    inline float lookupSin(float angle) {
        int idx = static_cast<int>((angle * FLOAT_SIN_SUBDIVISIONS)) % FLOAT_SIN_ANGLE_MAX;
        if (idx < 0) idx += FLOAT_SIN_ANGLE_MAX;
        return float_sin_table[idx];
    }

    inline float lookupCos(float angle) {
        int idx = static_cast<int>((angle + 90.0f) * FLOAT_SIN_SUBDIVISIONS) % FLOAT_SIN_ANGLE_MAX;
        if (idx < 0) idx += FLOAT_SIN_ANGLE_MAX;
        return float_sin_table[idx];
    }

    inline float lookupTan(float angle) {
        #if FLOAT_TAN_CACHE_SCALE > 1
            int idx = static_cast<int>((angle * FLOAT_TAN_SUBDIVISIONS)) % FLOAT_TAN_ANGLE_MAX;
            if (idx < 0) idx += FLOAT_TAN_ANGLE_MAX;
            return float_tan_table[idx];
        #else
            // Use integer lookup when scale is 1
            return static_cast<float>(lookupTanI(static_cast<int>(angle))) / FIXED_POINT_SCALE;
        #endif
    }
    #endif

} // namespace Renderer

#endif // TRIGLUT_HPP
