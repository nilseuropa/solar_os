#ifndef CAMERA_HPP
#define CAMERA_HPP

#include <cstdint>
#include "TrigLUT.hpp"
#include "Shader.hpp"
#include "Object.hpp"
#include "JetConfig.hpp"

namespace Renderer {

/// @brief View / projection source for the renderer.
///
/// Holds world-space position, Euler rotation, FOV and near/far clipping
/// distances. Rotation precision is selected at compile time via
/// FLOAT_CAMERA_ANGLES; integer angles are kept in degrees on the
/// [0, ANGLE_MAX) range and converted through the trig LUTs.
class Camera {
public:
    Vector3 position = {0,0,0};
    #if FLOAT_CAMERA_ANGLES
    Vector3_f rotation = {0,0,0};
    #else
    Vector3 rotation = {0,0,0};
    #endif
    float   fov;                ///< Field of view in degrees.
    int32_t nearPlane = 128;    ///< Near clip distance. Values below 128 can degrade perspective-correct texture mapping.
    int32_t farPlane = 1024;    ///< Far clip distance.

    int32_t halfFOV;            ///< Cached half-FOV in degrees (integer setFOV path only).
    int32_t tanHalfFOV;         ///< Cached tan(halfFOV) fixed-point (integer setFOV path only).
    float   fovFactor;          ///< Cached projection scale factor: (screenWidth/2) / tan(fov/2 radians).

    /// @brief Construct a camera with a default 60 degree FOV.
    Camera();

    /// @brief Set the world-space position.
    /// @param x World X coordinate.
    /// @param y World Y coordinate.
    /// @param z World Z coordinate.
    void setPosition(int32_t x, int32_t y, int32_t z);
    /// @brief Set the world-space position.
    /// @param pos World-space position vector.
    void setPosition(const Vector3& pos);

    /// @brief Set the absolute Euler rotation (degrees).
    /// @param rotX Rotation about X (pitch).
    /// @param rotY Rotation about Y (yaw).
    /// @param rotZ Rotation about Z (roll).
    void setRotation(int32_t rotX, int32_t rotY, int32_t rotZ);
    /// @brief Set the absolute Euler rotation (degrees).
    /// @param rot Rotation vector (X=pitch, Y=yaw, Z=roll).
    void setRotation(const Vector3& rot);

    #if FLOAT_CAMERA_ANGLES
    /// @brief Add a delta rotation in world axes (degrees, float build).
    /// @param rotX Delta about world X.
    /// @param rotY Delta about world Y.
    /// @param rotZ Delta about world Z.
    void rotate(float rotX, float rotY, float rotZ);
    /// @brief Add a delta rotation in world axes (degrees, float build).
    /// @param rot Delta rotation vector.
    void rotate(const Vector3_f& rot);
    /// @brief Add a delta rotation in the camera's local axes (float build).
    /// @param rotX Delta about local X.
    /// @param rotY Delta about local Y.
    /// @param rotZ Delta about local Z.
    void rotateLocal(float rotX, float rotY, float rotZ);
    /// @brief Add a delta rotation in the camera's local axes (float build).
    /// @param rot Delta rotation vector.
    void rotateLocal(const Vector3_f& rot);
    #else
    /// @brief Add a delta rotation in world axes (degrees).
    /// @param rotX Delta about world X.
    /// @param rotY Delta about world Y.
    /// @param rotZ Delta about world Z.
    void rotate(int32_t rotX, int32_t rotY, int32_t rotZ);
    /// @brief Add a delta rotation in world axes (degrees).
    /// @param rot Delta rotation vector.
    void rotate(const Vector3& rot);
    /// @brief Add a delta rotation in the camera's local axes (degrees).
    /// @param rotX Delta about local X.
    /// @param rotY Delta about local Y.
    /// @param rotZ Delta about local Z.
    void rotateLocal(int32_t rotX, int32_t rotY, int32_t rotZ);
    /// @brief Add a delta rotation in the camera's local axes (degrees).
    /// @param rot Delta rotation vector.
    void rotateLocal(const Vector3& rot);
    #endif

    /// @brief Translate the camera in world space.
    /// @param dx Delta X.
    /// @param dy Delta Y.
    /// @param dz Delta Z.
    void translate(int32_t dx, int32_t dy, int32_t dz);

    /// @brief Translate the camera along its local axes.
    /// @param dx Delta along local X (right).
    /// @param dy Delta along local Y (up).
    /// @param dz Delta along local Z (forward).
    void translateLocal(int32_t dx, int32_t dy, int32_t dz);
    /// @brief Translate the camera along its local axes.
    /// @param translation Local-space translation vector.
    void translateLocal(Vector3 translation);

    /// @brief Translate along local X/Z but keep Y world-aligned (yaw-only locomotion).
    /// @param dx Delta along yaw-relative right.
    /// @param dy Delta along world Y.
    /// @param dz Delta along yaw-relative forward.
    void translateLocalY(int32_t dx, int32_t dy, int32_t dz);
    /// @brief Translate along local X/Z but keep Y world-aligned (yaw-only locomotion).
    /// @param translation Translation vector.
    void translateLocalY(Vector3 translation);

    /// @brief Set the FOV and recompute cached projection factors (integer degrees).
    /// @param fovDegrees Vertical field of view in whole degrees.
    /// @param screenWidth Output width in pixels (used to derive fovFactor).
    void setFOV(int32_t fovDegrees, int32_t screenWidth);

    /// @brief Set the FOV and recompute cached projection factors (sub-degree precision).
    /// Uses tanf() instead of the integer LUT for smooth animated FOV transitions.
    /// @param fovDegrees Vertical field of view in degrees (may be fractional).
    /// @param screenWidth Output width in pixels (used to derive fovFactor).
    void setFOV(float fovDegrees, int32_t screenWidth);

    /// @brief Orient the camera so its forward vector points at a target.
    /// @param target World-space point to look at.
    void lookAt(const Vector3& target);
    /// @brief Orient the camera so its forward vector points at an object's centre.
    /// @param targetObject Object whose position+centreVolume is the target.
    void lookAt(const Object* targetObject);
    /// @brief Look-at restricted to the X (pitch) axis only.
    /// @param target World-space point.
    void lookAtX(const Vector3& target);
    /// @brief Look-at restricted to the X (pitch) axis only.
    /// @param targetObject Object to face.
    void lookAtX(const Object* targetObject);
    /// @brief Look-at restricted to the Y (yaw) axis only.
    /// @param target World-space point.
    void lookAtY(const Vector3& target);
    /// @brief Look-at restricted to the Y (yaw) axis only.
    /// @param targetObject Object to face.
    void lookAtY(const Object* targetObject);

    /// @brief Get the camera's combined rotation as sin/cos pairs per axis.
    /// @param cosX Out: cosine of pitch.
    /// @param sinX Out: sine of pitch.
    /// @param cosY Out: cosine of yaw.
    /// @param sinY Out: sine of yaw.
    /// @param cosZ Out: cosine of roll.
    /// @param sinZ Out: sine of roll.
    void getRotationMatrix(int32_t& cosX, int32_t& sinX,
                          int32_t& cosY, int32_t& sinY,
                          int32_t& cosZ, int32_t& sinZ) const;

    /// @brief Transform a direction vector from world to view space.
    /// @param dir World-space direction.
    /// @return Direction in camera/view space.
    Vector3 transformDirection(const Vector3& dir) const;
};

} // namespace Renderer

#endif // CAMERA_HPP
