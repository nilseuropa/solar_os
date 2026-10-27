#include "Camera.hpp"
#include "Object.hpp"
#include <vector>
#include <cstdint>
#include <cmath>
#include <Shader.hpp>

namespace Renderer
{

    Camera::Camera()
        : fov(60.0f)
    {
        setFOV(fov, 800); // Initialize cached values
    } // Default FOV to 60 degrees

    void Camera::setPosition(int32_t x, int32_t y, int32_t z)
    {
        position.assign(x, y, z);
    }

    void Camera::setPosition(const Vector3 &pos)
    {
        position.assign(pos);
    }

    void Camera::setRotation(int32_t rotX, int32_t rotY, int32_t rotZ)
    {
#if FLOAT_CAMERA_ANGLES
        rotation.assign(
            (float)rotX * (2.0f * M_PI) / ANGLE_MAX,
            (float)rotY * (2.0f * M_PI) / ANGLE_MAX,
            (float)rotZ * (2.0f * M_PI) / ANGLE_MAX);
#else
        rotation.x = rotX % ANGLE_MAX;
        rotation.y = rotY % ANGLE_MAX;
        rotation.z = rotZ % ANGLE_MAX;
#endif
    }

    void Camera::setRotation(const Vector3 &rot)
    {
#if FLOAT_CAMERA_ANGLES
        rotation.assign(
            (float)rot.x * (2.0f * M_PI) / ANGLE_MAX,
            (float)rot.y * (2.0f * M_PI) / ANGLE_MAX,
            (float)rot.z * (2.0f * M_PI) / ANGLE_MAX);
#else
        rotation.assign(rot);
#endif
    }

#if FLOAT_CAMERA_ANGLES
    void Camera::rotate(float rotX, float rotY, float rotZ)
    {
        rotation.x += (float)rotX * (2.0f * M_PI) / ANGLE_MAX;
        rotation.y += (float)rotY * (2.0f * M_PI) / ANGLE_MAX;
        rotation.z += (float)rotZ * (2.0f * M_PI) / ANGLE_MAX;
        // Normalize angles to [0, 2π]
        while (rotation.x >= 2.0f * M_PI)
            rotation.x -= 2.0f * M_PI;
        while (rotation.y >= 2.0f * M_PI)
            rotation.y -= 2.0f * M_PI;
        while (rotation.z >= 2.0f * M_PI)
            rotation.z -= 2.0f * M_PI;
        while (rotation.x < 0)
            rotation.x += 2.0f * M_PI;
        while (rotation.y < 0)
            rotation.y += 2.0f * M_PI;
        while (rotation.z < 0)
            rotation.z += 2.0f * M_PI;
    }

    void Camera::rotate(const Vector3_f &rot)
    {
        rotate(rot.x, rot.y, rot.z);
    }

    void Camera::rotateLocal(float rotX, float rotY, float rotZ)
    {
        // Convert input angles to radians
        float localX = rotX * (2.0f * M_PI) / ANGLE_MAX;
        float localY = rotY * (2.0f * M_PI) / ANGLE_MAX;
        float localZ = rotZ * (2.0f * M_PI) / ANGLE_MAX;

        // Add rotations directly since we're already in radians
        rotation.x += localX;
        rotation.y += localY;
        rotation.z += localZ;

        // Normalize angles to [0, 2π]
        while (rotation.x >= 2.0f * M_PI) rotation.x -= 2.0f * M_PI;
        while (rotation.y >= 2.0f * M_PI) rotation.y -= 2.0f * M_PI;
        while (rotation.z >= 2.0f * M_PI) rotation.z -= 2.0f * M_PI;
        while (rotation.x < 0) rotation.x += 2.0f * M_PI;
        while (rotation.y < 0) rotation.y += 2.0f * M_PI;
        while (rotation.z < 0) rotation.z += 2.0f * M_PI;
    }

    void Camera::rotateLocal(const Vector3_f &rot)
    {
        rotateLocal(rot.x, rot.y, rot.z);
    }
#else
    void Camera::rotate(int32_t rotX, int32_t rotY, int32_t rotZ)
    {
        rotation.x = (rotation.x + rotX) % ANGLE_MAX;
        rotation.y = (rotation.y + rotY) % ANGLE_MAX;
        rotation.z = (rotation.z + rotZ) % ANGLE_MAX;
    }

    void Camera::rotate(const Vector3 &rot)
    {
        rotation.add(rot);
        rotation.x %= ANGLE_MAX;
        rotation.y %= ANGLE_MAX;
        rotation.z %= ANGLE_MAX;
    }

    void Camera::rotateLocal(int32_t rotX, int32_t rotY, int32_t rotZ)
    {
        rotation.x = (rotation.x + rotX) % ANGLE_MAX;
        if (rotation.x < 0) rotation.x += ANGLE_MAX;

        rotation.y = (rotation.y + rotY) % ANGLE_MAX;
        if (rotation.y < 0) rotation.y += ANGLE_MAX;

        rotation.z = (rotation.z + rotZ) % ANGLE_MAX;
        if (rotation.z < 0) rotation.z += ANGLE_MAX;
    }

    void Camera::rotateLocal(const Vector3 &rot)
    {
        rotateLocal(rot.x, rot.y, rot.z);
    }
#endif


    void Camera::translate(int32_t dx, int32_t dy, int32_t dz)
    {
        position.add(dx, dy, dz);
    }

    void Camera::lookAt(const Vector3 &target)
    {
        // Calculate the direction vector from the camera to the target
        Vector3 direction = target - position;

// Calculate the rotation angles to look at the target in 3D space
#if FLOAT_CAMERA_ANGLES
        rotation.x = -std::atan2(direction.y, std::sqrt((int64_t)direction.x * direction.x + (int64_t)direction.z * direction.z));
        rotation.y = std::atan2(direction.x, direction.z);
        rotation.z = 0;
#else
        rotation.x = -static_cast<int32_t>(std::atan2(direction.y, std::sqrt((int64_t)direction.x * direction.x + (int64_t)direction.z * direction.z)) * ANGLE_MAX / (2.0f * M_PI));
        rotation.y = static_cast<int32_t>(std::atan2(direction.x, direction.z) * ANGLE_MAX / (2.0f * M_PI));
        rotation.z = 0;
#endif
    }

    void Camera::lookAt(const Object *targetObject)
    {
        lookAt(targetObject->position);
    }

    void Camera::lookAtX(const Vector3 &target)
    {
        // Calculate the direction vector from the camera to the target
        Vector3 direction = target - position;

// Calculate the rotation angles to look at the target in 3D space
#if FLOAT_CAMERA_ANGLES
        rotation.x = -std::atan2(direction.y, std::sqrt((int64_t)direction.x * direction.x + (int64_t)direction.z * direction.z));
        //rotation.y = 0;
        rotation.z = 0;
#else
        rotation.x = -static_cast<int32_t>(std::atan2(direction.y, std::sqrt((int64_t)direction.x * direction.x + (int64_t)direction.z * direction.z)) * ANGLE_MAX / (2.0f * M_PI));
        //rotation.y = 0;
        rotation.z = 0;
#endif
    }

    void Camera::lookAtX(const Object *targetObject)
    {
        lookAtX(targetObject->position);
    }

    void Camera::lookAtY(const Vector3 &target)
    {
        // Calculate the direction vector from the camera to the target
        Vector3 direction = target - position;

// Calculate the rotation angles to look at the target in 3D space
#if FLOAT_CAMERA_ANGLES
        //rotation.x = 0;
        rotation.y = std::atan2(direction.x, direction.z);
        rotation.z = 0;
#else
        //rotation.x = 0;
        rotation.y = static_cast<int32_t>(std::atan2(direction.x, direction.z) * ANGLE_MAX / (2.0f * M_PI));
        rotation.z = 0;
#endif
    }

    void Camera::lookAtY(const Object *targetObject)
    {
        lookAtY(targetObject->position);
    }

    void Camera::translateLocal(int32_t dx, int32_t dy, int32_t dz)
    {
        #if FLOAT_CAMERA_ANGLES
            float angleX = rotation.x;
            float angleY = rotation.y;
            float cosX = std::cos(angleX);
            float sinX = std::sin(angleX);
            float cosY = std::cos(angleY);
            float sinY = std::sin(angleY);

            // First rotate around X (pitch)
            float rotatedY = dy * cosX - dz * sinX;
            float rotatedZ = dy * sinX + dz * cosX;

            // Then rotate around Y (yaw)
            Vector3_f localTranslation(
                dx * cosY + rotatedZ * sinY,
                rotatedY,
                -dx * sinY + rotatedZ * cosY);

            position.add(localTranslation.x, localTranslation.y, localTranslation.z);
        #else
            int32_t angleX = rotation.x;
            int32_t angleY = rotation.y;
            int32_t cosX = lookupCosI(angleX);
            int32_t sinX = lookupSinI(angleX);
            int32_t cosY = lookupCosI(angleY);
            int32_t sinY = lookupSinI(angleY);

            // First rotate around X (pitch)
            int32_t rotatedY = (dy * cosX - dz * sinX) / FIXED_POINT_SCALE;
            int32_t rotatedZ = (dy * sinX + dz * cosX) / FIXED_POINT_SCALE;

            // Then rotate around Y (yaw)
            Vector3 localTranslation(
                (dx * cosY + rotatedZ * sinY) / FIXED_POINT_SCALE,
                rotatedY,
                (-dx * sinY + rotatedZ * cosY) / FIXED_POINT_SCALE);

            position.add(localTranslation.x, localTranslation.y, localTranslation.z);
        #endif
    }

    void Camera::translateLocal(Vector3 translation)
    {
        translateLocal(translation.x, translation.y, translation.z);
    }

    void Camera::translateLocalY(int32_t dx, int32_t dy, int32_t dz)
    {
        //Translate the camera position in local space but only relative to its rotation on the Y axis.
        //This is useful for moving the camera forward/backward relative to its current direction.
        #if FLOAT_CAMERA_ANGLES
            float angleY = rotation.y;
            float cosY = std::cos(angleY);
            float sinY = std::sin(angleY);
            Vector3_f localTranslation(
                dx * cosY + dz * sinY,
                dy,
                -dx * sinY + dz * cosY);
            position.add(localTranslation.x, localTranslation.y, localTranslation.z);
        #else
            int32_t angleY = rotation.y;
            int32_t cosY = lookupCosI(angleY);
            int32_t sinY = lookupSinI(angleY);
            Vector3 localTranslation(
                (dx * cosY + dz * sinY) / FIXED_POINT_SCALE,
                dy,
                (-dx * sinY + dz * cosY) / FIXED_POINT_SCALE);
            position.add(localTranslation.x, localTranslation.y, localTranslation.z);
        #endif
    }

    void Camera::translateLocalY(Vector3 translation)
    {
        translateLocalY(translation.x, translation.y, translation.z);
    }

    void Camera::setFOV(int32_t fovDegrees, int32_t screenWidth)
    {
        // Delegate to float overload so both paths share one implementation.
        setFOV(static_cast<float>(fovDegrees), screenWidth);
        // Keep legacy integer cached fields in sync for any existing callers.
        halfFOV = static_cast<int32_t>(fov) / 2;
        if (halfFOV >= ANGLE_MAX) halfFOV %= ANGLE_MAX;
        tanHalfFOV = lookupTanI(halfFOV);
        if (tanHalfFOV == 0) tanHalfFOV = 1;
    }

    void Camera::setFOV(float fovDegrees, int32_t screenWidth)
    {
        fov = fovDegrees;
        // half-FOV in radians: fov/2 * pi/180 = fov * pi/360
        const float halfFovRad = fovDegrees * (static_cast<float>(M_PI) / 360.0f);
        const float t = tanf(halfFovRad);
        // fovFactor = (screenWidth/2) / tan(fov/2).
        // Projection: screenX = pos.x * fovFactor / pos.z + screenWidth/2
        fovFactor = (t > 0.0f) ? (static_cast<float>(screenWidth / 2) / t) : 1.0f;
    }

    void Camera::getRotationMatrix(int32_t& cosX, int32_t& sinX,
                             int32_t& cosY, int32_t& sinY,
                             int32_t& cosZ, int32_t& sinZ) const {
    #if FLOAT_CAMERA_ANGLES
    float angleXf = -rotation.x * 180.0f / M_PI;
    float angleYf = -rotation.y * 180.0f / M_PI;
    float angleZf = -rotation.z * 180.0f / M_PI;
    cosX = lookupCos(angleXf) * FIXED_POINT_SCALE;
    sinX = lookupSin(angleXf) * FIXED_POINT_SCALE;
    cosY = lookupCos(angleYf) * FIXED_POINT_SCALE;
    sinY = lookupSin(angleYf) * FIXED_POINT_SCALE;
    cosZ = lookupCos(angleZf) * FIXED_POINT_SCALE;
    sinZ = lookupSin(angleZf) * FIXED_POINT_SCALE;
    #else
    int32_t angleX = (ANGLE_MAX - rotation.x) % ANGLE_MAX;
    int32_t angleY = (ANGLE_MAX - rotation.y) % ANGLE_MAX;
    int32_t angleZ = (ANGLE_MAX - rotation.z) % ANGLE_MAX;
    cosX = lookupCosI(angleX);
    sinX = lookupSinI(angleX);
    cosY = lookupCosI(angleY);
    sinY = lookupSinI(angleY);
    cosZ = lookupCosI(angleZ);
    sinZ = lookupSinI(angleZ);
    #endif
}

Vector3 Camera::transformDirection(const Vector3& dir) const {
    Vector3 result = dir;
    int32_t cosX, sinX, cosY, sinY, cosZ, sinZ;
    getRotationMatrix(cosX, sinX, cosY, sinY, cosZ, sinZ);

    // Y rotation first (yaw)
    result.assign(
        (result.x * cosY + result.z * sinY) / FIXED_POINT_SCALE,
        result.y,
        (-result.x * sinY + result.z * cosY) / FIXED_POINT_SCALE);

    // X rotation second (pitch)
    result.assign(result.x,
        (result.y * cosX - result.z * sinX) / FIXED_POINT_SCALE,
        (result.y * sinX + result.z * cosX) / FIXED_POINT_SCALE);

    // Z rotation last (roll - usually 0 for FPS camera)
    result.assign(
        (result.x * cosZ - result.y * sinZ) / FIXED_POINT_SCALE,
        (result.x * sinZ + result.y * cosZ) / FIXED_POINT_SCALE,
        result.z);

    return result;
}

} // namespace Renderer
