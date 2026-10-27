#ifndef JET_MATH_HPP
#define JET_MATH_HPP

/// @file Math.hpp
/// @brief Lightweight float vector type and spline utilities used throughout
///        the Jet engine.

#include <cmath>
#include <algorithm>

namespace Renderer {

// ---------------------------------------------------------------------------
/// @brief Simple 3-component float vector.
///
/// Used for world-space positions, velocities, and spline tangents.
/// Integer-precision Renderer::Object vertices use the engine's own
/// int32_t-based Vector3; Vec3f is for construction-time and physics math
/// where float precision is preferable.
// ---------------------------------------------------------------------------
struct Vec3f {
    float x, y, z;

    Vec3f() : x(0), y(0), z(0) {}
    Vec3f(float x, float y, float z) : x(x), y(y), z(z) {}

    Vec3f operator+(const Vec3f& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3f operator-(const Vec3f& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3f operator*(float s)        const { return {x * s,   y * s,   z * s  }; }
    Vec3f operator/(float s)        const { return {x / s,   y / s,   z / s  }; }

    Vec3f& operator+=(const Vec3f& o) { x += o.x; y += o.y; z += o.z; return *this; }

    float dot(const Vec3f& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3f cross(const Vec3f& o) const {
        return { y * o.z - z * o.y,
                 z * o.x - x * o.z,
                 x * o.y - y * o.x };
    }
    float length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3f normalize() const {
        float l = length();
        return l > 1e-6f ? *this / l : Vec3f{0, 1, 0};
    }
};

// ---------------------------------------------------------------------------
// Catmull-Rom spline helpers. Passes through control points with C1
// continuity. Given four nodes p0,p1,p2,p3 and t ∈ [0,1], returns the
// curve position between p1 and p2 (p0 and p3 are tangent influence nodes).
// ---------------------------------------------------------------------------
inline Vec3f catmullRom(const Vec3f& p0, const Vec3f& p1,
                        const Vec3f& p2, const Vec3f& p3, float t)
{
    float t2 = t * t, t3 = t2 * t;
    return  (p0 * (-0.5f * t3 + t2 - 0.5f * t))
          + (p1 * ( 1.5f * t3 - 2.5f * t2 + 1.0f))
          + (p2 * (-1.5f * t3 + 2.0f * t2 + 0.5f * t))
          + (p3 * ( 0.5f * t3 - 0.5f * t2));
}

inline Vec3f catmullRomTangent(const Vec3f& p0, const Vec3f& p1,
                               const Vec3f& p2, const Vec3f& p3, float t)
{
    float t2 = t * t;
    return  (p0 * (-1.5f * t2 + 2.0f * t - 0.5f))
          + (p1 * ( 4.5f * t2 - 5.0f * t))
          + (p2 * (-4.5f * t2 + 4.0f * t + 0.5f))
          + (p3 * ( 1.5f * t2 - t));
}

} // namespace Renderer

#endif // JET_MATH_HPP
