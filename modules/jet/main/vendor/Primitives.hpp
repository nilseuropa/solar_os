#ifndef PRIMITIVES_HPP
#define PRIMITIVES_HPP

#include "Object.hpp"
#include "Material.hpp"

namespace Primitives {
using namespace Renderer;

/// @brief Build an axis-aligned box mesh.
/// @param width Size along X.
/// @param height Size along Y.
/// @param depth Size along Z.
/// @param material Material assigned to every face.
/// @return Newly-allocated Object owned by the caller.
Object* createCube(int32_t width, int32_t height, int32_t depth, Material* material);

/// @brief Build a 6-coloured debug cube (one colour per face) for orientation visualisation.
/// @param width Size along X.
/// @param height Size along Y.
/// @param depth Size along Z.
/// @return Newly-allocated Object owned by the caller.
Object* createDebugCube(int32_t width, int32_t height, int32_t depth);

/// @brief Build a tessellated XZ grid mesh.
/// @param width Total width along X.
/// @param height Total depth along Z.
/// @param rows Number of cell rows along Z.
/// @param cols Number of cell columns along X.
/// @param material Material applied to even cells.
/// @param material2 Material applied to odd cells (checkerboard).
/// @param perCellUV When true, each cell gets a fresh [0,1] UV span; otherwise UVs span the whole grid.
/// @return Newly-allocated Object owned by the caller.
Object* createGrid(int32_t width, int32_t height, int32_t rows, int32_t cols, Material* material, Material* material2, bool perCellUV = false);

/// @brief Build a flat XZ plane mesh.
/// @param width Size along X.
/// @param height Size along Z.
/// @param material Material applied to the plane.
/// @return Newly-allocated Object owned by the caller.
Object* createPlane(int32_t width, int32_t height, Material* material);

/// @brief Build a square-based pyramid mesh.
/// @param baseSize Size of the square base.
/// @param height Height along Y.
/// @param material Material applied to every face.
/// @return Newly-allocated Object owned by the caller.
Object* createPyramid(int32_t baseSize, int32_t height, Material* material);

/// @brief Build a UV-sphere mesh.
/// @param radius Sphere radius.
/// @param segments Number of horizontal/vertical subdivisions.
/// @param material Material applied to every face.
/// @param uScale UV scaling along U.
/// @param vScale UV scaling along V.
/// @return Newly-allocated Object owned by the caller.
Object* createSphere(int32_t radius, int32_t segments, Material* material, int32_t uScale = 1, int32_t vScale = 1);

/// @brief Build a capsule (cylinder with hemispherical caps) mesh.
/// @param radius Capsule radius.
/// @param height Total height including caps.
/// @param segments Radial subdivisions.
/// @param material Material applied to every face.
/// @return Newly-allocated Object owned by the caller.
Object* createCapsule(int32_t radius, int32_t height, int32_t segments, Material* material);

/// @brief Build a cylinder mesh.
/// @param radius Cylinder radius.
/// @param height Cylinder height along Y.
/// @param segments Radial subdivisions.
/// @param closedCaps When true, top and bottom caps are generated.
/// @param material Material applied to every face.
/// @return Newly-allocated Object owned by the caller.
Object* createCylinder(int32_t radius, int32_t height, int32_t segments, bool closedCaps, Material* material);

/// @brief Build a camera-facing billboard quad (auto-rotated at render time).
/// @param width Quad width.
/// @param height Quad height.
/// @param material Material applied to the quad.
/// @return Newly-allocated Object owned by the caller, with isBillboard set.
Object* createBillboard(int32_t width, int32_t height, Material* material);

/// @brief Build a fixed-orientation XY quad.
/// @param width Quad width along X.
/// @param height Quad height along Y.
/// @param material Material applied to the quad.
/// @return Newly-allocated Object owned by the caller.
Object* createQuad(int32_t width, int32_t height, Material* material);

} // namespace Primitives

#endif // PRIMITIVES_HPP
