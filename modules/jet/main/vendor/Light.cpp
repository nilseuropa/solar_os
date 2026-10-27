#include "Light.hpp"
#include "Shader.hpp"
#include "TrigLUT.hpp"

namespace Renderer {

DirectionalLight::DirectionalLight(Vector3 direction, Color color, uint16_t intensity)
    : direction(direction), color(color), intensity(intensity) {
    calculateLightDirection();
}

void DirectionalLight::updateDirection(Vector3 newDirection) {
    direction = newDirection;
    calculateLightDirection();
}

void DirectionalLight::calculateLightDirection() {
    // The trig tables are normally initialised by Scene::Scene(), but
    // a DirectionalLight can be constructed before any Scene exists (e.g.
    // as a struct member alongside a Scene*).  Guard against that here so
    // worldLightDir is always valid regardless of construction order.
    if (sin_table[90] == 0) initializeTrigTables();
    int32_t azimuthDeg = (direction.x + ANGLE_MAX) % ANGLE_MAX; // Keep within 0-359 range
    int32_t elevationDeg = (direction.y + ANGLE_MAX) % ANGLE_MAX; // Ignore direction.z

    // Calculate world space light direction vector
    worldLightDir.x = (lookupCosI(elevationDeg) * lookupCosI(azimuthDeg)) >> FIXED_POINT_SHIFT;
    worldLightDir.y = lookupSinI(elevationDeg);
    worldLightDir.z = (lookupCosI(elevationDeg) * lookupSinI(azimuthDeg)) >> FIXED_POINT_SHIFT;

    // Initially set view space direction same as world space
    lightDir = worldLightDir;
}

void DirectionalLight::updateViewSpaceDirection(const Camera* camera) {
    // Transform world space direction to view space
    lightDir = camera->transformDirection(worldLightDir);
}

PointLight::PointLight(Vector3 position, Color color, uint16_t rangeMin, uint16_t rangeMax)
    : position(position), color(color), rangeMin(rangeMin), rangeMax(rangeMax) {}

AmbientLight::AmbientLight(Color color)
    : color(color) {}

} // namespace Renderer
