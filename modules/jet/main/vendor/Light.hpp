#ifndef LIGHT_HPP
#define LIGHT_HPP

#include <cstdint>
#include "Shader.hpp"
#include "Camera.hpp"

namespace Renderer {

/// @brief 24-bit (8/8/8) RGB colour used by the lighting types.
struct Color {
    uint8_t r, g, b;
};

/// @brief Infinite-distance light defined by an azimuth/elevation direction.
///
/// `direction` is interpreted as integer-degree (azimuth, elevation) pair
/// in `direction.x` and `direction.y`; `worldLightDir` and `lightDir` are
/// the derived unit-length vectors in world and view space respectively.
class DirectionalLight {
public:
    Vector3 direction;          ///< Source angles (x=azimuth deg, y=elevation deg).
    Color color;                ///< Light colour.
    uint16_t intensity;         ///< Linear intensity multiplier (0..255).

    Vector3 worldLightDir;      ///< Derived world-space direction vector.
    Vector3 lightDir;           ///< Derived view-space direction vector (filled by updateViewSpaceDirection).

    /// @brief Construct a directional light.
    /// @param direction Source angles (x=azimuth deg, y=elevation deg).
    /// @param color Light colour.
    /// @param intensity Linear intensity (0..255).
    DirectionalLight(Vector3 direction, Color color, uint16_t intensity = 255);

    /// @brief Update the source angles and recompute the world direction.
    /// @param newDirection New (azimuth, elevation) angles.
    void updateDirection(Vector3 newDirection);

    /// @brief Recompute the view-space direction from the current camera rotation.
    /// @param camera Camera to take the rotation from.
    void updateViewSpaceDirection(const Camera* camera);

private:
    void calculateLightDirection();
};

/// @brief Point light with a linear falloff between rangeMin and rangeMax.
class PointLight {
public:
    Vector3 position;           ///< World-space position.
    Color color;                ///< Light colour.
    uint16_t rangeMin;          ///< Distance at which the light is at full intensity.
    uint16_t rangeMax;          ///< Distance at which the light's contribution reaches zero.
    bool enabled = true;        ///< Per-instance enable flag.

    /// @brief Construct a point light.
    /// @param position World-space position.
    /// @param color Light colour.
    /// @param rangeMin Distance at which the light is full intensity.
    /// @param rangeMax Distance at which the light fades to zero.
    PointLight(Vector3 position, Color color, uint16_t rangeMin = 0, uint16_t rangeMax = 255);
};

/// @brief Constant ambient term added to every shaded pixel.
class AmbientLight {
public:
    Color color;                ///< Ambient colour.

    /// @brief Construct an ambient light.
    /// @param color Ambient colour to add to the scene.
    AmbientLight(Color color);
};

} // namespace Renderer

#endif // LIGHT_HPP
