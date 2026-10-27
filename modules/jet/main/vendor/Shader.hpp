// Shader.hpp
#ifndef SHADER_HPP
#define SHADER_HPP

#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>
#include "FastMath.hpp"
#include "TrigLUT.hpp"

using namespace Renderer;

// Forward declarations for types used in shader parameters
struct Vector3
{
    int32_t x, y, z;

    // Empty constructor with 0 values
    Vector3() : x(0), y(0), z(0) {}
    Vector3(int32_t x, int32_t y, int32_t z) : x(x), y(y), z(z) {}

    Vector3 add(const Vector3 &other) const
    {
        return Vector3(x + other.x, y + other.y, z + other.z);
    }

    void add(const Vector3 &other)
    {
        x += other.x;
        y += other.y;
        z += other.z;
    }

    Vector3 add(int32_t x, int32_t y, int32_t z) const
    {
        return Vector3(this->x + x, this->y + y, this->z + z);
    }

    void add(int32_t x, int32_t y, int32_t z)
    {
        this->x += x;
        this->y += y;
        this->z += z;
    }

    Vector3 multiply(const Vector3 &other) const
    {
        return Vector3(x * other.x, y * other.y, z * other.z);
    }

    Vector3 multiply(int32_t scalar) const
    {
        return Vector3(x * scalar, y * scalar, z * scalar);
    }

    Vector3 divide(int32_t scalar) const
    {
        return Vector3(x / scalar, y / scalar, z / scalar);
    }

    Vector3 divide(const Vector3 &other) const
    {
        return Vector3(x / other.x, y / other.y, z / other.z);
    }

    Vector3 subtract(const Vector3 &other) const
    {
        return Vector3(x - other.x, y - other.y, z - other.z);
    }

    Vector3 cross(const Vector3 &other) const
    {
        return Vector3(y * other.z - z * other.y,
                       z * other.x - x * other.z,
                       x * other.y - y * other.x);
    }

    // static cross product
    static Vector3 cross(const Vector3 &a, const Vector3 &b)
    {
        return Vector3(a.y * b.z - a.z * b.y,
                       a.z * b.x - a.x * b.z,
                       a.x * b.y - a.y * b.x);
    }

    static int64_t dotProduct(const Vector3 &a, const Vector3 &b)
    {
        return FastMath::dotProduct(a.x, a.y, a.z, b.x, b.y, b.z);
    }

    Vector3 inverse() const
    {
        return Vector3(-x, -y, -z);
    }

    int32_t length() const
    {
        return static_cast<int32_t>(FastMath::approximateSqrt(x * x + y * y + z * z));
    }

    void assign(int32_t x, int32_t y, int32_t z)
    {
        this->x = x;
        this->y = y;
        this->z = z;
    }

    void assign(const Vector3 &other)
    {
        this->x = other.x;
        this->y = other.y;
        this->z = other.z;
    }

    Vector3 operator+(const Vector3 &other) const
    {
        return add(other);
    }

    Vector3 operator*(const Vector3 &other) const
    {
        return multiply(other);
    }

    Vector3 operator*(int32_t scalar) const
    {
        return multiply(scalar);
    }

    Vector3 operator-(const Vector3 &other) const
    {
        return subtract(other);
    }

    Vector3 operator/(int32_t scalar) const
    {
        return divide(scalar);
    }

    Vector3 operator/(const Vector3 &other) const
    {
        return divide(other);
    }

    Vector3 operator%(int32_t scalar) const
    {
        return Vector3(x % scalar, y % scalar, z % scalar);
    }
};

struct Vector3_f
{
    float x, y, z;

    Vector3_f() : x(0.0f), y(0.0f), z(0.0f) {}
    Vector3_f(float x, float y, float z) : x(x), y(y), z(z) {}
    Vector3_f(const Vector3_f &other) : x(other.x), y(other.y), z(other.z) {}
    Vector3_f(const Vector3 &other) : x(static_cast<float>(other.x)), y(static_cast<float>(other.y)), z(static_cast<float>(other.z)) {}

    Vector3_f add(const Vector3_f &other) const { return Vector3_f(x + other.x, y + other.y, z + other.z); }
    Vector3_f add(const Vector3 &other) const { return Vector3_f(x + other.x, y + other.y, z + other.z); }
    Vector3_f add(float x, float y, float z) const { return Vector3_f(this->x + x, this->y + y, this->z + z); }

    Vector3_f multiply(const Vector3_f &other) const { return Vector3_f(x * other.x, y * other.y, z * other.z); }
    Vector3_f multiply(const Vector3 &other) const { return Vector3_f(x * other.x, y * other.y, z * other.z); }
    Vector3_f multiply(float scalar) const { return Vector3_f(x * scalar, y * scalar, z * scalar); }

    Vector3_f divide(float scalar) const { return Vector3_f(x / scalar, y / scalar, z / scalar); }
    Vector3_f divide(const Vector3_f &other) const { return Vector3_f(x / other.x, y / other.y, z / other.z); }
    Vector3_f divide(const Vector3 &other) const { return Vector3_f(x / other.x, y / other.y, z / other.z); }

    Vector3_f subtract(const Vector3_f &other) const { return Vector3_f(x - other.x, y - other.y, z - other.z); }
    Vector3_f cross(const Vector3_f &other) const { return Vector3_f(y * other.z - z * other.y, z * other.x - x * other.z, x * other.y - y * other.x); }

    static Vector3_f cross(const Vector3_f &a, const Vector3_f &b) { return Vector3_f(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
    static float dotProduct(const Vector3_f &a, const Vector3_f &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    Vector3_f inverse() const { return Vector3_f(-x, -y, -z); }
    float length() const { return std::sqrt(x * x + y * y + z * z); }

    void assign(float x, float y, float z) { this->x = x; this->y = y; this->z = z; }
    void assign(const Vector3_f &other) { x = other.x; y = other.y; z = other.z; }

    Vector3_f operator+(const Vector3_f &other) const { return add(other); }
    Vector3_f operator+(const Vector3 &other) const { return add(other); }
    Vector3_f operator*(const Vector3_f &other) const { return multiply(other); }
    Vector3_f operator*(const Vector3 &other) const { return multiply(other); }
    Vector3_f operator*(float scalar) const { return multiply(scalar); }
    Vector3_f operator-(const Vector3_f &other) const { return subtract(other); }
    Vector3_f operator/(float scalar) const { return divide(scalar); }
    Vector3_f operator/(const Vector3_f &other) const { return divide(other); }
    Vector3_f operator/(const Vector3 &other) const { return divide(other); }
};

struct Vector3_64
{
    int64_t x, y, z;
    Vector3_64(int64_t x, int64_t y, int64_t z) : x(x), y(y), z(z) {}

    Vector3_64 add(const Vector3_64 &other) const
    {
        return Vector3_64(x + other.x, y + other.y, z + other.z);
    }

    Vector3_64 add(int64_t x, int64_t y, int64_t z) const
    {
        return Vector3_64(this->x + x, this->y + y, this->z + z);
    }

    Vector3_64 multiply(const Vector3_64 &other) const
    {
        return Vector3_64(x * other.x, y * other.y, z * other.z);
    }

    Vector3_64 multiply(int64_t scalar) const
    {
        return Vector3_64(x * scalar, y * scalar, z * scalar);
    }

    void assign(int64_t x, int64_t y, int64_t z)
    {
        this->x = x;
        this->y = y;
        this->z = z;
    }

    void assign(const Vector3_64 &other)
    {
        this->x = other.x;
        this->y = other.y;
        this->z = other.z;
    }

    int64_t length() const
    {
        return static_cast<int64_t>(FastMath::approximateSqrt(x * x + y * y + z * z));
    }

    Vector3_64 cross(const Vector3_64 &other) const
    {
        return Vector3_64(y * other.z - z * other.y,
                          z * other.x - x * other.z,
                          x * other.y - y * other.x);
    }

    // static cross product
    static Vector3_64 cross(const Vector3_64 &a, const Vector3_64 &b)
    {
        return Vector3_64(a.y * b.z - a.z * b.y,
                          a.z * b.x - a.x * b.z,
                          a.x * b.y - a.y * b.x);
    }

    Vector3_64 operator+(const Vector3_64 &other) const
    {
        return add(other);
    }

    Vector3_64 operator*(const Vector3_64 &other) const
    {
        return multiply(other);
    }

    Vector3_64 operator*(int64_t scalar) const
    {
        return multiply(scalar);
    }

    // Interoperability with Vector3
    Vector3_64(const Vector3 &other) : x(other.x), y(other.y), z(other.z) {}

    Vector3_64 add(const Vector3 &other) const
    {
        return Vector3_64(x + other.x, y + other.y, z + other.z);
    }

    Vector3_64 multiply(const Vector3 &other) const
    {
        return Vector3_64(x * other.x, y * other.y, z * other.z);
    }

    Vector3_64 multiply(int32_t scalar) const
    {
        return Vector3_64(x * scalar, y * scalar, z * scalar);
    }

    Vector3_64 divide(int32_t scalar) const
    {
        return Vector3_64(x / scalar, y / scalar, z / scalar);
    }

    Vector3_64 divide(const Vector3 &other) const
    {
        return Vector3_64(x / other.x, y / other.y, z / other.z);
    }

    Vector3_64 operator+(const Vector3 &other) const
    {
        return add(other);
    }

    Vector3_64 operator*(const Vector3 &other) const
    {
        return multiply(other);
    }

    Vector3_64 operator*(int32_t scalar) const
    {
        return multiply(scalar);
    }

    Vector3_64 operator/(int32_t scalar) const
    {
        return divide(scalar);
    }

    Vector3_64 operator/(const Vector3 &other) const
    {
        return divide(other);
    }

    static int64_t dotProduct(const Vector3_64 &a, const Vector3_64 &b)
    {
        return FastMath::dotProduct(a.x, a.y, a.z, b.x, b.y, b.z);
    }
};

struct Vector2
{
    int32_t x, y;
    Vector2(int32_t x, int32_t y) : x(x), y(y) {}
    Vector2() : x(0), y(0) {}

    Vector2 add(const Vector2 &other) const
    {
        return Vector2(x + other.x, y + other.y);
    }

    Vector2 multiply(const Vector2 &other) const
    {
        return Vector2(x * other.x, y * other.y);
    }

    Vector2 multiply(int32_t scalar) const
    {
        return Vector2(x * scalar, y * scalar);
    }

    void assign(int32_t x, int32_t y)
    {
        this->x = x;
        this->y = y;
    }
};

struct Vector2_f {
    float x, y;
    Vector2_f(float x, float y) : x(x), y(y) {}
    Vector2_f() : x(0.0f), y(0.0f) {}

    Vector2_f add(const Vector2_f &other) const { return Vector2_f(x + other.x, y + other.y); }
    Vector2_f multiply(const Vector2_f &other) const { return Vector2_f(x * other.x, y * other.y); }
    Vector2_f multiply(float scalar) const { return Vector2_f(x * scalar, y * scalar); }

    void assign(float x, float y) { this->x = x; this->y = y; }
};

struct Quaternion
{
    int32_t w, x, y, z; // Fixed-point components

    Quaternion() : w(FIXED_POINT_SCALE), x(0), y(0), z(0) {}
    Quaternion(int32_t w, int32_t x, int32_t y, int32_t z) : w(w), x(x), y(y), z(z) {}

    // Normalize the quaternion
    void normalize()
    {
        int64_t magnitude = FastMath::approximateSqrt(w * w + x * x + y * y + z * z);
        w = (w * FIXED_POINT_SCALE) / magnitude;
        x = (x * FIXED_POINT_SCALE) / magnitude;
        y = (y * FIXED_POINT_SCALE) / magnitude;
        z = (z * FIXED_POINT_SCALE) / magnitude;
    }

    // Multiply two quaternions
    Quaternion multiply(const Quaternion &other) const
    {
        Quaternion result;
        result.w = ((w * other.w - x * other.x - y * other.y - z * other.z) / FIXED_POINT_SCALE);
        result.x = ((w * other.x + x * other.w + y * other.z - z * other.y) / FIXED_POINT_SCALE);
        result.y = ((w * other.y - x * other.z + y * other.w + z * other.x) / FIXED_POINT_SCALE);
        result.z = ((w * other.z + x * other.y - y * other.x + z * other.w) / FIXED_POINT_SCALE);
        return result;
    }

    // Rotate a vector by this quaternion
    Vector3 rotate(const Vector3 &v) const
    {
        Quaternion p(0, v.x, v.y, v.z);
        Quaternion q = multiply(p).multiply(conjugate());
        return Vector3(q.x, q.y, q.z);
    }

    // Conjugate the quaternion
    Quaternion conjugate() const
    {
        return Quaternion(w, -x, -y, -z);
    }

    static Quaternion fromEuler(const Vector3 &angles)
    {
        return fromEuler(angles.x, angles.y, angles.z);
    }

    // Convert from Euler angles (in degrees)
    static Quaternion fromEuler(int32_t pitch, int32_t yaw, int32_t roll)
    {
        pitch %= 360;
        yaw %= 360;
        roll %= 360;
        if (pitch < 0)
            pitch += 360;
        if (yaw < 0)
            yaw += 360;
        if (roll < 0)
            roll += 360;

        int32_t cy = lookupCosI(yaw / 2);
        int32_t sy = lookupSinI(yaw / 2);
        int32_t cp = lookupCosI(pitch / 2);
        int32_t sp = lookupSinI(pitch / 2);
        int32_t cr = lookupCosI(roll / 2);
        int32_t sr = lookupSinI(roll / 2);

        Quaternion q;
        q.w = ((cy * cp * cr + sy * sp * sr) / FIXED_POINT_SCALE);
        q.x = ((cy * cp * sr - sy * sp * cr) / FIXED_POINT_SCALE);
        q.y = ((sy * cp * sr + cy * sp * cr) / FIXED_POINT_SCALE);
        q.z = ((sy * cp * cr - cy * sp * sr) / FIXED_POINT_SCALE);
        return q;
    }

    // Convert to Euler angles (in degrees)
    void toEuler(int32_t &pitch, int32_t &yaw, int32_t &roll) const
    {
        int32_t sinr_cosp = 2 * (w * x + y * z) / FIXED_POINT_SCALE;
        int32_t cosr_cosp = FIXED_POINT_SCALE - 2 * (x * x + y * y);
        roll = static_cast<int32_t>(std::atan2(sinr_cosp, cosr_cosp) * 180.0 / M_PI);

        int32_t sinp = 2 * (w * y - z * x) / FIXED_POINT_SCALE;
        if (std::abs(sinp) >= FIXED_POINT_SCALE)
            pitch = (sinp > 0) ? 90 : -90;
        else
            pitch = static_cast<int32_t>(std::asin(sinp) * 180.0 / M_PI);

        int32_t siny_cosp = 2 * (w * z + x * y) / FIXED_POINT_SCALE;
        int32_t cosy_cosp = FIXED_POINT_SCALE - 2 * (y * y + z * z);
        yaw = static_cast<int32_t>(std::atan2(siny_cosp, cosy_cosp) * 180.0 / M_PI);
    }

    void toEuler(Vector3 &angles) const
    {
        toEuler(angles.x, angles.y, angles.z);
    }
};

struct Quaternion_f {
    float w, x, y, z;

    Quaternion_f() : w(1.0f), x(0.0f), y(0.0f), z(0.0f) {}
    Quaternion_f(float w, float x, float y, float z) : w(w), x(x), y(y), z(z) {}

    void normalize() {
        float magnitude = std::sqrt(w * w + x * x + y * y + z * z);
        w /= magnitude;
        x /= magnitude;
        y /= magnitude;
        z /= magnitude;
    }

    Quaternion_f multiply(const Quaternion_f &other) const {
        return Quaternion_f(
            w * other.w - x * other.x - y * other.y - z * other.z,
            w * other.x + x * other.w + y * other.z - z * other.y,
            w * other.y - x * other.z + y * other.w + z * other.x,
            w * other.z + x * other.y - y * other.x + z * other.w
        );
    }

    Vector3_f rotate(const Vector3_f &v) const {
        Quaternion_f p(0, v.x, v.y, v.z);
        Quaternion_f q = multiply(p).multiply(conjugate());
        return Vector3_f(q.x, q.y, q.z);
    }

    Quaternion_f conjugate() const {
        return Quaternion_f(w, -x, -y, -z);
    }

    static Quaternion_f fromEuler(const Vector3_f &angles) {
        return fromEuler(angles.x, angles.y, angles.z);
    }

    static Quaternion_f fromEuler(float pitch, float yaw, float roll) {
        pitch = fmodf(pitch, 360.0f);
        yaw = fmodf(yaw, 360.0f);
        roll = fmodf(roll, 360.0f);

        float cy = cosf(yaw * 0.5f * M_PI / 180.0f);
        float sy = sinf(yaw * 0.5f * M_PI / 180.0f);
        float cp = cosf(pitch * 0.5f * M_PI / 180.0f);
        float sp = sinf(pitch * 0.5f * M_PI / 180.0f);
        float cr = cosf(roll * 0.5f * M_PI / 180.0f);
        float sr = sinf(roll * 0.5f * M_PI / 180.0f);

        return Quaternion_f(
            cy * cp * cr + sy * sp * sr,
            cy * cp * sr - sy * sp * cr,
            sy * cp * sr + cy * sp * cr,
            sy * cp * cr - cy * sp * sr
        );
    }

    void toEuler(float &pitch, float &yaw, float &roll) const {
        float sinr_cosp = 2.0f * (w * x + y * z);
        float cosr_cosp = 1.0f - 2.0f * (x * x + y * y);
        roll = atan2f(sinr_cosp, cosr_cosp) * 180.0f / M_PI;

        float sinp = 2.0f * (w * y - z * x);
        pitch = std::abs(sinp) >= 1.0f ?
            copysignf(90.0f, sinp) :
            asinf(sinp) * 180.0f / M_PI;

        float siny_cosp = 2.0f * (w * z + x * y);
        float cosy_cosp = 1.0f - 2.0f * (y * y + z * z);
        yaw = atan2f(siny_cosp, cosy_cosp) * 180.0f / M_PI;
    }

    void toEuler(Vector3_f &angles) const {
        toEuler(angles.x, angles.y, angles.z);
    }
};

struct ShaderParameters
{
    Vector2 screenPos;
    int32_t zBuffer = 0;
    Vector3 localCoords;
    Vector3 worldCoords;
    uint32_t frameNumber = 0;
};

// Only shader faces retain these inputs. Original view-space vertices preserve
// local/world interpolation through clipping, billboards and mesh instances.
struct ShaderFaceContext {
    Vector3 local[3], view[3], cameraPosition;
    float cameraMatrix[9];
    ShaderParameters parameters(int x,int y,int depth,int width,int height,float fov,uint32_t frame) const {
        ShaderParameters p;p.screenPos={x,y};p.zBuffer=depth;p.frameNumber=frame;
        const Vector3_f a(view[0]),e1(Vector3_f(view[1])-a),e2(Vector3_f(view[2])-a);
        const Vector3_f ray((x-width/2)/fov,(height/2-y)/fov,1);
        const Vector3_f n=Vector3_f::cross(e1,e2);
        const float nd=Vector3_f::dotProduct(n,ray);
        if(std::abs(nd)<1e-12f)return p;
        const Vector3_f hit=ray*(Vector3_f::dotProduct(n,a)/nd),d=hit-a;
        const double aa=Vector3_f::dotProduct(e1,e1),ab=Vector3_f::dotProduct(e1,e2),bb=Vector3_f::dotProduct(e2,e2);
        const double det=aa*bb-ab*ab;if(std::abs(det)<1e-12)return p;
        const double da=Vector3_f::dotProduct(d,e1),db=Vector3_f::dotProduct(d,e2);
        const double u=(da*bb-db*ab)/det,v=(db*aa-da*ab)/det;
        auto integer=[](double value) {return int32_t(std::max(double(INT32_MIN),std::min(double(INT32_MAX),value)));};
        p.localCoords={integer(local[0].x+u*(local[1].x-local[0].x)+v*(local[2].x-local[0].x)),
            integer(local[0].y+u*(local[1].y-local[0].y)+v*(local[2].y-local[0].y)),
            integer(local[0].z+u*(local[1].z-local[0].z)+v*(local[2].z-local[0].z))};
        p.worldCoords={integer(cameraPosition.x+cameraMatrix[0]*hit.x+cameraMatrix[3]*hit.y+cameraMatrix[6]*hit.z),
            integer(cameraPosition.y+cameraMatrix[1]*hit.x+cameraMatrix[4]*hit.y+cameraMatrix[7]*hit.z),
            integer(cameraPosition.z+cameraMatrix[2]*hit.x+cameraMatrix[5]*hit.y+cameraMatrix[8]*hit.z)};
        return p;
    }
};

class Shader
{
public:
    virtual ~Shader() {}

    bool screenSpace = false;
    bool needsCoordinates = true;
    bool hasAlpha = false;       // Indicates if an alpha color is used
    uint16_t alphaColor = 0; // The color to treat as transparent

    /**
     * @brief Computes the color of a pixel based on various parameters.
     *
     * @param u Texture coordinate U, 0-1023.
     * @param v Texture coordinate V, 0-1023.
     * @param params Additional parameters for shading.
     * @return uint16_t The resulting color in RGB565 format.
     */
    virtual uint16_t getPixel(uint32_t u, uint32_t v, ShaderParameters *params = nullptr) = 0;
};

#endif // SHADER_HPP
