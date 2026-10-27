#include "Material.hpp"
#include "Shader.hpp"

namespace Renderer {

Material::Material(uint16_t color, Texture* diffuseMap, Shader* shader, bool emissive, uint8_t alpha, uint8_t diffuse, uint8_t specular)
    : color(color), diffuseMap(diffuseMap), shader(shader), emissive(emissive), alpha(alpha), diffuse(diffuse), specular(specular) {

}

Material::Material(uint16_t color, uint8_t alpha)
    : color(color), diffuseMap(nullptr), shader(nullptr), emissive(false), alpha(alpha), diffuse(255) {

}

uint16_t Material::getColor(Vector2 uv) {
    return getColor(uv.x, uv.y);
}

inline uint16_t Material::getColor(int u, int v) {
    #if !TEXTURE_MAPPING
    return color;
    #endif

    if (diffuseMap) {
        return diffuseMap->getPixel(u, v);
    } else {
        return color;
    }
}

} // namespace Renderer
