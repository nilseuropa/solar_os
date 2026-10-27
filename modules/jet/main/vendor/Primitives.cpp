#include "Primitives.hpp"
#include "TrigLUT.hpp"
#include "Shader.hpp"
#include <cmath>
#include <memory>

namespace Primitives
{
    void computeNormal(Object *obj, uint16_t idx1, uint16_t idx2, uint16_t idx3)
    {
        Object::Vertex &v1 = obj->vertices[idx1];
        Object::Vertex &v2 = obj->vertices[idx2];
        Object::Vertex &v3 = obj->vertices[idx3];

        Vector3 u = v2.position - v1.position;
        Vector3 v = v3.position - v1.position;
        Vector3 n = u.cross(v);

        // Normalize the normal vector to maintain consistent lighting
        int32_t length = n.length();
        if (length == 0)
            length = 1; // Avoid division by zero

        n.assign(n.x * FIXED_POINT_SCALE / length,
                 n.y * FIXED_POINT_SCALE / length,
                 n.z * FIXED_POINT_SCALE / length);

        v1.normal.assign(n.x, n.y, n.z);
    }

    Object *createCube(int32_t width, int32_t height, int32_t depth, Material *material)
    {
        std::unique_ptr<Object> cube(new Object());

        int32_t hw = width / 2;
        int32_t hh = height / 2;
        int32_t hd = depth / 2;

        uint16_t maxU = FIXED_POINT_SCALE;
        uint16_t maxV = FIXED_POINT_SCALE;

        // Define the vertices for each face of the cube
        // Front face
        cube->addVertex({{-hw, -hh, hd}, {0, maxV}, {0, 0, FIXED_POINT_SCALE}});   // v0
        cube->addVertex({{hw, -hh, hd}, {maxU, maxV}, {0, 0, FIXED_POINT_SCALE}}); // v1
        cube->addVertex({{hw, hh, hd}, {maxU, 0}, {0, 0, FIXED_POINT_SCALE}});     // v2
        cube->addVertex({{-hw, hh, hd}, {0, 0}, {0, 0, FIXED_POINT_SCALE}});       // v3

        // Back face
        cube->addVertex({{-hw, -hh, -hd}, {0, maxV}, {0, 0, -FIXED_POINT_SCALE}});   // v4
        cube->addVertex({{hw, -hh, -hd}, {maxU, maxV}, {0, 0, -FIXED_POINT_SCALE}}); // v5
        cube->addVertex({{hw, hh, -hd}, {maxU, 0}, {0, 0, -FIXED_POINT_SCALE}});     // v6
        cube->addVertex({{-hw, hh, -hd}, {0, 0}, {0, 0, -FIXED_POINT_SCALE}});       // v7

        // Left face
        cube->addVertex({{-hw, -hh, -hd}, {0, maxV}, {-FIXED_POINT_SCALE, 0, 0}});   // v8
        cube->addVertex({{-hw, -hh, hd}, {maxU, maxV}, {-FIXED_POINT_SCALE, 0, 0}}); // v9
        cube->addVertex({{-hw, hh, hd}, {maxU, 0}, {-FIXED_POINT_SCALE, 0, 0}});     // v10
        cube->addVertex({{-hw, hh, -hd}, {0, 0}, {-FIXED_POINT_SCALE, 0, 0}});       // v11

        // Right face
        cube->addVertex({{hw, -hh, hd}, {0, maxV}, {FIXED_POINT_SCALE, 0, 0}});     // v12
        cube->addVertex({{hw, -hh, -hd}, {maxU, maxV}, {FIXED_POINT_SCALE, 0, 0}}); // v13
        cube->addVertex({{hw, hh, -hd}, {maxU, 0}, {FIXED_POINT_SCALE, 0, 0}});     // v14
        cube->addVertex({{hw, hh, hd}, {0, 0}, {FIXED_POINT_SCALE, 0, 0}});         // v15

        // Top face
        cube->addVertex({{-hw, hh, hd}, {0, maxV}, {0, FIXED_POINT_SCALE, 0}});   // v16
        cube->addVertex({{hw, hh, hd}, {maxU, maxV}, {0, FIXED_POINT_SCALE, 0}}); // v17
        cube->addVertex({{hw, hh, -hd}, {maxU, 0}, {0, FIXED_POINT_SCALE, 0}});   // v18
        cube->addVertex({{-hw, hh, -hd}, {0, 0}, {0, FIXED_POINT_SCALE, 0}});     // v19

        // Bottom face
        cube->addVertex({{-hw, -hh, -hd}, {0, maxV}, {0, -FIXED_POINT_SCALE, 0}});   // v20
        cube->addVertex({{hw, -hh, -hd}, {maxU, maxV}, {0, -FIXED_POINT_SCALE, 0}}); // v21
        cube->addVertex({{hw, -hh, hd}, {maxU, 0}, {0, -FIXED_POINT_SCALE, 0}});     // v22
        cube->addVertex({{-hw, -hh, hd}, {0, 0}, {0, -FIXED_POINT_SCALE, 0}});       // v23

        // Add faces
        cube->addFace(0, 1, 2, 3, material);     // Front face
        cube->addFace(5, 4, 7, 6, material);     // Back face
        cube->addFace(8, 9, 10, 11, material);   // Left face
        cube->addFace(12, 13, 14, 15, material); // Right face
        cube->addFace(16, 17, 18, 19, material); // Top face
        cube->addFace(20, 21, 22, 23, material); // Bottom face

        cube->calculateBoundingBox();

        return cube.release();
    }

    Object *createDebugCube(int32_t width, int32_t height, int32_t depth)
    {
        Object *cube = new Object();

        int32_t hw = width / 2;
        int32_t hh = height / 2;
        int32_t hd = depth / 2;

        // 24 verts: 4 per face, each carrying its own face normal. The
        // previous 8-vertex layout shared corners across 6 faces but only
        // stamped a ±Z normal on each, leaving 4 of 6 faces with garbage
        // normals (rendered as ambient-only under LIGHTING).
        const int32_t N = FIXED_POINT_SCALE;

        // Front (+Z)
        cube->addVertex({{-hw, -hh,  hd}, {0, 0}, {0, 0,  N}});
        cube->addVertex({{ hw, -hh,  hd}, {0, 0}, {0, 0,  N}});
        cube->addVertex({{ hw,  hh,  hd}, {0, 0}, {0, 0,  N}});
        cube->addVertex({{-hw,  hh,  hd}, {0, 0}, {0, 0,  N}});
        // Back (-Z)
        cube->addVertex({{ hw, -hh, -hd}, {0, 0}, {0, 0, -N}});
        cube->addVertex({{-hw, -hh, -hd}, {0, 0}, {0, 0, -N}});
        cube->addVertex({{-hw,  hh, -hd}, {0, 0}, {0, 0, -N}});
        cube->addVertex({{ hw,  hh, -hd}, {0, 0}, {0, 0, -N}});
        // Left (-X)
        cube->addVertex({{-hw, -hh, -hd}, {0, 0}, {-N, 0, 0}});
        cube->addVertex({{-hw, -hh,  hd}, {0, 0}, {-N, 0, 0}});
        cube->addVertex({{-hw,  hh,  hd}, {0, 0}, {-N, 0, 0}});
        cube->addVertex({{-hw,  hh, -hd}, {0, 0}, {-N, 0, 0}});
        // Right (+X)
        cube->addVertex({{ hw, -hh,  hd}, {0, 0}, { N, 0, 0}});
        cube->addVertex({{ hw, -hh, -hd}, {0, 0}, { N, 0, 0}});
        cube->addVertex({{ hw,  hh, -hd}, {0, 0}, { N, 0, 0}});
        cube->addVertex({{ hw,  hh,  hd}, {0, 0}, { N, 0, 0}});
        // Top (+Y)
        cube->addVertex({{-hw,  hh,  hd}, {0, 0}, {0,  N, 0}});
        cube->addVertex({{ hw,  hh,  hd}, {0, 0}, {0,  N, 0}});
        cube->addVertex({{ hw,  hh, -hd}, {0, 0}, {0,  N, 0}});
        cube->addVertex({{-hw,  hh, -hd}, {0, 0}, {0,  N, 0}});
        // Bottom (-Y)
        cube->addVertex({{-hw, -hh, -hd}, {0, 0}, {0, -N, 0}});
        cube->addVertex({{ hw, -hh, -hd}, {0, 0}, {0, -N, 0}});
        cube->addVertex({{ hw, -hh,  hd}, {0, 0}, {0, -N, 0}});
        cube->addVertex({{-hw, -hh,  hd}, {0, 0}, {0, -N, 0}});

        Material *frontMaterial  = new Material(0xF800, nullptr, nullptr, false); // Red
        Material *backMaterial   = new Material(0x07E0, nullptr, nullptr, false); // Green
        Material *leftMaterial   = new Material(0x001F, nullptr, nullptr, false); // Blue
        Material *rightMaterial  = new Material(0xFFE0, nullptr, nullptr, false); // Yellow
        Material *topMaterial    = new Material(0xF81F, nullptr, nullptr, false); // Magenta
        Material *bottomMaterial = new Material(0x07FF, nullptr, nullptr, false); // Cyan

        cube->addFace( 0,  1,  2,  3, frontMaterial);
        cube->addFace( 4,  5,  6,  7, backMaterial);
        cube->addFace( 8,  9, 10, 11, leftMaterial);
        cube->addFace(12, 13, 14, 15, rightMaterial);
        cube->addFace(16, 17, 18, 19, topMaterial);
        cube->addFace(20, 21, 22, 23, bottomMaterial);

        cube->calculateBoundingBox();

        return cube;
    }

    Object *createGrid(int32_t width, int32_t height, int32_t rows, int32_t cols, Material *material, Material *material2, bool perCellUV)
    {
        std::unique_ptr<Object> grid(new Object());

        int32_t hw = width / 2;
        int32_t hh = height / 2;

        // Calculate the spacing between rows and columns
        int32_t rowSpacing = height / rows;
        int32_t colSpacing = width / cols;

        // Define vertices for the grid
        for (int32_t r = 0; r < rows; ++r)
        {
            for (int32_t c = 0; c < cols; ++c)
            {
                int32_t x = c * colSpacing - hw;
                int32_t y = r * rowSpacing - hh;

                Vector2 uv;
                if (perCellUV) {
                    // UV coordinates that repeat for each cell (0 to FIXED_POINT_SCALE)
                    uv = {
                        (c % 2) * FIXED_POINT_SCALE,
                        (r % 2) * FIXED_POINT_SCALE
                    };
                } else {
                    // Original UV mapping across entire grid
                    uv = {
                        c * FIXED_POINT_SCALE / (cols - 1),
                        r * FIXED_POINT_SCALE / (rows - 1)
                    };
                }
                grid->addVertex({{x, 0, y}, uv, {0, FIXED_POINT_SCALE, 0}});
            }
        }

        // Create faces for the grid
        for (int32_t r = 0; r < rows - 1; ++r)
        {
            for (int32_t c = 0; c < cols - 1; ++c)
            {
                int32_t v0 = r * cols + c;
                int32_t v1 = v0 + 1;
                int32_t v2 = v1 + cols;
                int32_t v3 = v0 + cols;

                //Use alternating materials for each cell
                grid->addFace(v0, v1, v2, v3, (r + c) % 2 == 0 ? material : material2);
            }
        }

        grid->calculateBoundingBox();

        return grid.release();
    }

    Object *createPlane(int32_t width, int32_t height, Material *material)
    {
        std::unique_ptr<Object> plane(new Object());

        int32_t hw = width / 2;
        int32_t hh = height / 2;

        // Define vertices (lying on the XZ plane at Y = 0)
        plane->addVertex({{-hw, 0, -hh}, {0, 0}, {0, FIXED_POINT_SCALE, 0}}); // v0
        plane->addVertex({{hw, 0, -hh}, {FIXED_POINT_SCALE, 0}, {0, FIXED_POINT_SCALE, 0}});  // v1
        plane->addVertex({{hw, 0, hh}, {FIXED_POINT_SCALE, FIXED_POINT_SCALE}, {0, FIXED_POINT_SCALE, 0}});   // v2
        plane->addVertex({{-hw, 0, hh}, {0, FIXED_POINT_SCALE}, {0, FIXED_POINT_SCALE, 0}});  // v3

        plane->addFace(0, 1, 2, 3, material);

        plane->calculateBoundingBox();

        return plane.release();
    }

    Object *createPyramid(int32_t baseSize, int32_t height, Material *material)
    {
        std::unique_ptr<Object> pyramid(new Object());

        int32_t halfBase = baseSize / 2;
        const int32_t N = FIXED_POINT_SCALE;

        // 4 base verts + 4 sides × 3 verts = 16 verts. Per-face vertex
        // ownership avoids the destructive shared-vertex normal stamping
        // the previous implementation had: it used 5 shared verts and
        // overwrote each base corner's downward normal with whichever
        // side face happened to reference it last, leaving the base
        // tris with garbage normals and the apex with no normal at all.
        Vector2 apexUV = {FIXED_POINT_SCALE / 2, FIXED_POINT_SCALE / 2};

        // Base (-Y), CCW from below so the normal points down.
        pyramid->addVertex({{-halfBase, 0, -halfBase}, {0, 0},                                       {0, -N, 0}});
        pyramid->addVertex({{ halfBase, 0,  halfBase}, {FIXED_POINT_SCALE, FIXED_POINT_SCALE},       {0, -N, 0}});
        pyramid->addVertex({{ halfBase, 0, -halfBase}, {FIXED_POINT_SCALE, 0},                       {0, -N, 0}});
        pyramid->addVertex({{-halfBase, 0,  halfBase}, {0, FIXED_POINT_SCALE},                       {0, -N, 0}});
        pyramid->addTriangle(0, 1, 2, material);
        pyramid->addTriangle(0, 3, 1, material);

        // Four side faces. Each gets its own three verts so the apex can
        // hold per-side normals — `computeFlatNormals` then assigns the
        // real cross-product face normal to all three.
        auto addSide = [&](int32_t x0, int32_t z0, int32_t x1, int32_t z1) {
            uint16_t b = (uint16_t)pyramid->vertices.size();
            pyramid->addVertex({{x0, 0, z0},         {0, 0},                                 {0, 0, 0}});
            pyramid->addVertex({{x1, 0, z1},         {FIXED_POINT_SCALE, 0},                 {0, 0, 0}});
            pyramid->addVertex({{ 0, height, 0},     apexUV,                                 {0, 0, 0}});
            pyramid->addTriangle(b, b + 1, b + 2, material);
        };
        // Wind each side CCW viewed from outside so outward normals win
        // backface culling.
        addSide(-halfBase, -halfBase,  halfBase, -halfBase); // front (-Z)
        addSide( halfBase, -halfBase,  halfBase,  halfBase); // right (+X)
        addSide( halfBase,  halfBase, -halfBase,  halfBase); // back (+Z)
        addSide(-halfBase,  halfBase, -halfBase, -halfBase); // left (-X)

        pyramid->computeFlatNormals();
        pyramid->calculateBoundingBox();

        return pyramid.release();
    }

    Object *createSphere(int32_t radius, int32_t segments, Material *material, int32_t uScale, int32_t vScale)
    {
        std::unique_ptr<Object> sphere(new Object());

        // Ensure at least 3 segments
        if (segments < 3)
            segments = 3;

        // Angles in fixed-point degrees
        int32_t angleStep = ANGLE_MAX / segments;

        uint16_t maxU = FIXED_POINT_SCALE;
        uint16_t maxV = FIXED_POINT_SCALE;

        // Generate vertices
        for (int32_t latAngle = 0; latAngle <= 180; latAngle += angleStep)
        {
            int32_t sinLat = lookupSinI(latAngle % ANGLE_MAX);
            int32_t cosLat = lookupCosI(latAngle % ANGLE_MAX);

            for (int32_t lonAngle = 0; lonAngle <= 360; lonAngle += angleStep)
            {
                int32_t sinLon = lookupSinI(lonAngle % ANGLE_MAX);
                int32_t cosLon = lookupCosI(lonAngle % ANGLE_MAX);

                // Calculate vertex position
                Vector3 vertexPosition = {int32_t(int64_t(radius) * sinLat * cosLon / FIXED_POINT_SCALE / FIXED_POINT_SCALE),
                                          radius * cosLat / FIXED_POINT_SCALE,
                                          int32_t(int64_t(radius) * sinLat * sinLon / FIXED_POINT_SCALE / FIXED_POINT_SCALE)};

                // Fixed UV coordinates
                Vector2 uv = {
                    (lonAngle * maxU * uScale) / 360,
                    (latAngle * maxV * vScale) / 180
                };

                // Normal vector (normalized position vector)
                Vector3 normal = vertexPosition * FIXED_POINT_SCALE / radius;

                sphere->addVertex({vertexPosition, uv, normal});
            }
        }

        // Generate faces
        int latSegments = 180 / angleStep;
        int lonSegments = 360 / angleStep;

        for (int lat = 0; lat < latSegments; ++lat)
        {
            for (int lon = 0; lon < lonSegments; ++lon)
            {
                int current = lat * (lonSegments + 1) + lon;
                int next = current + lonSegments + 1;

                // To wrap around, connect the last column with the first
                int v0 = current;
                int v1 = (lon == lonSegments - 1) ? current - lonSegments + 1 : current + 1;
                int v2 = (lon == lonSegments - 1) ? next - lonSegments + 1 : next + 1;
                int v3 = next;

                // Add a quad face using the four vertices
                sphere->addFace(v0, v1, v2, v3, material);
            }
        }

        sphere->calculateBoundingBox();

        return sphere.release();
    }

    Object *createCapsule(int32_t radius, int32_t height, int32_t segments, Material *material)
    {
        std::unique_ptr<Object> capsule(new Object());

        // Ensure at least 3 segments
        if (segments < 3)
            segments = 3;

        // Angles in fixed-point degrees
        int32_t angleStep = ANGLE_MAX / segments;

        uint16_t maxU = FIXED_POINT_SCALE;
        uint16_t maxV = FIXED_POINT_SCALE;

        // Generate vertices for the top hemisphere
        for (int32_t latAngle = 0; latAngle <= 90; latAngle += angleStep)
        {
            int32_t sinLat = lookupSinI(latAngle % ANGLE_MAX);
            int32_t cosLat = lookupCosI(latAngle % ANGLE_MAX);

            for (int32_t lonAngle = 0; lonAngle <= 360; lonAngle += angleStep)
            {
            int32_t sinLon = lookupSinI(lonAngle % ANGLE_MAX);
            int32_t cosLon = lookupCosI(lonAngle % ANGLE_MAX);

            // Calculate vertex position
            Vector3 vertexPosition = {int32_t(int64_t(radius) * sinLat * cosLon / FIXED_POINT_SCALE / FIXED_POINT_SCALE),
                          radius * cosLat / FIXED_POINT_SCALE + height / 2,
                          int32_t(int64_t(radius) * sinLat * sinLon / FIXED_POINT_SCALE / FIXED_POINT_SCALE)};

            // Fixed UV coordinates
            Vector2 uv = {
                (lonAngle * maxU) / 360,
                (latAngle * maxV) / 180
            };

            // Normal vector (normalized position vector)
            Vector3 normal = vertexPosition * FIXED_POINT_SCALE / radius;

            capsule->addVertex({vertexPosition, uv, normal});
            }
        }

        // Generate vertices for the cylinder part
        for (int32_t i = 0; i <= segments; ++i)
        {
            int32_t y = height / 2 - i * height / segments;

            for (int32_t lonAngle = 0; lonAngle <= 360; lonAngle += angleStep)
            {
            int32_t sinLon = lookupSinI(lonAngle % ANGLE_MAX);
            int32_t cosLon = lookupCosI(lonAngle % ANGLE_MAX);

            // Calculate vertex position
            Vector3 vertexPosition = {radius * cosLon / FIXED_POINT_SCALE,
                          y,
                          radius * sinLon / FIXED_POINT_SCALE};

            // Fixed UV coordinates
            Vector2 uv = {
                (lonAngle * maxU) / 360,
                (i * maxV) / segments
            };

            // Normal vector (normalized position vector)
            Vector3 normal = vertexPosition * FIXED_POINT_SCALE / radius;

            capsule->addVertex({vertexPosition, uv, normal});
            }
        }

        // Generate vertices for the bottom hemisphere
        for (int32_t latAngle = 90; latAngle <= 180; latAngle += angleStep)
        {
            int32_t sinLat = lookupSinI(latAngle % ANGLE_MAX);
            int32_t cosLat = lookupCosI(latAngle % ANGLE_MAX);

            for (int32_t lonAngle = 0; lonAngle <= 360; lonAngle += angleStep)
            {
            int32_t sinLon = lookupSinI(lonAngle % ANGLE_MAX);
            int32_t cosLon = lookupCosI(lonAngle % ANGLE_MAX);

            // Calculate vertex position
            Vector3 vertexPosition = {int32_t(int64_t(radius) * sinLat * cosLon / FIXED_POINT_SCALE / FIXED_POINT_SCALE),
                          radius * cosLat / FIXED_POINT_SCALE - height / 2,
                          int32_t(int64_t(radius) * sinLat * sinLon / FIXED_POINT_SCALE / FIXED_POINT_SCALE)};

            // Fixed UV coordinates
            Vector2 uv = {
                (lonAngle * maxU) / 360,
                (latAngle * maxV) / 180
            };

            // Normal vector (normalized position vector)
            Vector3 normal = vertexPosition * FIXED_POINT_SCALE / radius;

            capsule->addVertex({vertexPosition, uv, normal});
            }
        }

        // Generate faces for the top hemisphere
        int latSegments = 90 / angleStep;
        int lonSegments = 360 / angleStep;

        for (int lat = 0; lat < latSegments; ++lat)
        {
            for (int lon = 0; lon < lonSegments; ++lon)
            {
            int current = lat * (lonSegments + 1) + lon;
            int next = current + lonSegments + 1;

            // To wrap around, connect the last column with the first
            int v0 = current;
            int v1 = (lon == lonSegments - 1) ? current - lonSegments + 1 : current + 1;
            int v2 = (lon == lonSegments - 1) ? next - lonSegments + 1 : next + 1;
            int v3 = next;

            // Add a quad face using the four vertices
            capsule->addFace(v0, v1, v2, v3, material);
            }
        }

        // Generate faces for the cylinder part
        int offset = (latSegments + 1) * (lonSegments + 1);
        for (int i = 0; i < segments; ++i)
        {
            for (int lon = 0; lon < lonSegments; ++lon)
            {
            int current = offset + i * (lonSegments + 1) + lon;
            int next = current + lonSegments + 1;

            // To wrap around, connect the last column with the first
            int v0 = current;
            int v1 = (lon == lonSegments - 1) ? current - lonSegments + 1 : current + 1;
            int v2 = (lon == lonSegments - 1) ? next - lonSegments + 1 : next + 1;
            int v3 = next;

            // Add a quad face using the four vertices
            capsule->addFace(v0, v1, v2, v3, material);
            }
        }

        // Generate faces for the bottom hemisphere
        offset += (segments + 1) * (lonSegments + 1);
        for (int lat = 0; lat < latSegments; ++lat)
        {
            for (int lon = 0; lon < lonSegments; ++lon)
            {
            int current = offset + lat * (lonSegments + 1) + lon;
            int next = current + lonSegments + 1;

            // To wrap around, connect the last column with the first
            int v0 = current;
            int v1 = (lon == lonSegments - 1) ? current - lonSegments + 1 : current + 1;
            int v2 = (lon == lonSegments - 1) ? next - lonSegments + 1 : next + 1;
            int v3 = next;

            // Add a quad face using the four vertices
            capsule->addFace(v0, v1, v2, v3, material);
            }
        }

        capsule->calculateBoundingBox();

        return capsule.release();
    }

    Object *createCylinder(int32_t radius, int32_t height, int32_t segments, bool closedCaps, Material *material)
    {
        std::unique_ptr<Object> cylinder(new Object());

        // Ensure at least 3 segments
        if (segments < 3)
            segments = 3;

        // Angles in fixed-point degrees
        int32_t angleStep = ANGLE_MAX / segments;

        uint16_t maxU = FIXED_POINT_SCALE;
        uint16_t maxV = FIXED_POINT_SCALE;

        // Generate vertices for the top and bottom circles
        for (int32_t i = 0; i <= 1; ++i)
        {
            int32_t y = (i == 0) ? height / 2 : -height / 2;

            for (int32_t lonAngle = 0; lonAngle <= 360; lonAngle += angleStep)
            {
            int32_t sinLon = lookupSinI(lonAngle % ANGLE_MAX);
            int32_t cosLon = lookupCosI(lonAngle % ANGLE_MAX);

            // Calculate vertex position
            Vector3 vertexPosition = {radius * cosLon / FIXED_POINT_SCALE,
                          y,
                          radius * sinLon / FIXED_POINT_SCALE};

            // Fixed UV coordinates
            Vector2 uv = {
                (lonAngle * maxU) / 360,
                (i * maxV)
            };

            // Normal vector (normalized position vector)
            Vector3 normal = {cosLon, 0, sinLon};

            cylinder->addVertex({vertexPosition, uv, normal});
            }
        }

        // Generate vertices for the side faces
        for (int32_t i = 0; i <= segments; ++i)
        {
            int32_t y = height / 2 - i * height / segments;

            for (int32_t lonAngle = 0; lonAngle <= 360; lonAngle += angleStep)
            {
            int32_t sinLon = lookupSinI(lonAngle % ANGLE_MAX);
            int32_t cosLon = lookupCosI(lonAngle % ANGLE_MAX);

            // Calculate vertex position
            Vector3 vertexPosition = {radius * cosLon / FIXED_POINT_SCALE,
                          y,
                          radius * sinLon / FIXED_POINT_SCALE};

            // Fixed UV coordinates
            Vector2 uv = {
                (lonAngle * maxU) / 360,
                (i * maxV) / segments
            };

            // Normal vector (normalized position vector)
            Vector3 normal = {cosLon, 0, sinLon};

            cylinder->addVertex({vertexPosition, uv, normal});
            }
        }

        // Generate faces for the top and bottom circles
        int lonSegments = 360 / angleStep;

        if (closedCaps)
        {
            for (int lon = 0; lon < lonSegments; ++lon)
            {
            int v0 = lon;
            int v1 = (lon == lonSegments - 1) ? 0 : lon + 1;
            int v2 = lon + lonSegments + 1;
            int v3 = (lon == lonSegments - 1) ? lonSegments + 1 : lon + lonSegments + 2;

            // Top cap
            cylinder->addTriangle(v0, v1, lonSegments + 1, material);

            // Bottom cap
            cylinder->addTriangle(v2, v3, 2 * (lonSegments + 1), material);
            }
        }

        // Generate faces for the side faces
        int offset = 2 * (lonSegments + 1);
        for (int i = 0; i < segments; ++i)
        {
            for (int lon = 0; lon < lonSegments; ++lon)
            {
            int current = offset + i * (lonSegments + 1) + lon;
            int next = current + lonSegments + 1;

            // To wrap around, connect the last column with the first
            int v0 = current;
            int v1 = (lon == lonSegments - 1) ? current - lonSegments + 1 : current + 1;
            int v2 = (lon == lonSegments - 1) ? next - lonSegments + 1 : next + 1;
            int v3 = next;

            // Add a quad face using the four vertices
            cylinder->addFace(v0, v1, v2, v3, material);
            }
        }

        cylinder->calculateBoundingBox();

        return cylinder.release();
    }

    Object *createQuad(int32_t width, int32_t height, Material *material)
    {
        std::unique_ptr<Object> quad(new Object());

        // Define vertices for a plane centered at the origin
        int32_t hw = width / 2;
        int32_t hh = height / 2;

        uint16_t maxU = FIXED_POINT_SCALE;
        uint16_t maxV = FIXED_POINT_SCALE;

        quad->addVertex({{-hw, -hh, 0}, {0, 0},       {0, 0, FIXED_POINT_SCALE}}); // v0
        quad->addVertex({{ hw, -hh, 0}, {maxU, 0},    {0, 0, FIXED_POINT_SCALE}}); // v1
        quad->addVertex({{ hw,  hh, 0}, {maxU, maxV}, {0, 0, FIXED_POINT_SCALE}}); // v2
        quad->addVertex({{-hw,  hh, 0}, {0, maxV},    {0, 0, FIXED_POINT_SCALE}}); // v3

        // Define two triangles
        quad->addTriangle(0, 1, 2, material);
        quad->addTriangle(0, 2, 3, material);

        quad->calculateBoundingBox();

        return quad.release();
    }

    Object *createBillboard(int32_t width, int32_t height, Material *material)
    {
        Object *billboard = createQuad(width, height, material);
        billboard->isBillboard = true;
        return billboard;
    }

} // namespace Primitives
