#include "Scene.hpp"
#include "Primitives.hpp"
#include "ParticleSystem.hpp"
#include "LensFlare.hpp"
#include "Framebuffer.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>

using namespace Renderer;
struct FixtureShader:Shader {
    unsigned calls=0;
    uint16_t getPixel(uint32_t u,uint32_t v,ShaderParameters *p) override {
        assert(p && p->screenPos.x>=0 && p->screenPos.y>=0);++calls;
        return uint16_t(((u&1023)>>5)<<11|((v&1023)>>4)<<5|(p->frameNumber&31));
    }
};

// Compare raw output from native/wire-order builds outside the renderer. The
// fixture exercises actual scene transforms, clears, depth, shaders, textures,
// translucent framebuffer reads, reflection reads, sprites and scene teardown.
int main(int argc, char **argv) {
    assert(argc == 2);
    FILE *output = std::fopen(argv[1], "wb");
    assert(output);
    constexpr int width = 64, height = 48, count = width * height, guard = 16;
    constexpr uint16_t sentinel = 0xa15c;
    for (int lifetime = 0; lifetime < 3; ++lifetime) {
        std::vector<uint16_t> color(count + guard * 2, sentinel);
        std::vector<uint16_t> depth(count + guard * 2, sentinel);
        std::vector<uint16_t> reflection(count);
        std::array<uint16_t, height> gradient;
        for (int y = 0; y < height; ++y) gradient[y] = uint16_t(0x1234 + y * 35);
        uint16_t texels[] = {0xf800, 0x07e0, 0x001f, 0xffe0};
        Texture texture(2, 2, texels);
        std::array<uint8_t,1024> tileIndices{};
        tileIndices[0]=0;tileIndices[1]=1;tileIndices[32]=2;tileIndices[33]=3;
        TileTexture tileView;tileView.backing=tileIndices.data();tileView.palette=texels;
        tileView.width=tileView.height=2;tileView.pagesX=1;tileView.direct=true;
        uint16_t linearIndices[]={0x0100,0x0302};
        Texture tiledTexture(2,2,linearIndices);tiledTexture.palette=texels;tiledTexture.paletteSize=4;
        tiledTexture.tiled=&tileView;tiledTexture.addressMode=CLAMP;
        Material material(0xe94b), overlay(0x3b62), textured(0xffff, &texture);
        FixtureShader shader;
        material.specular = 20;
        Camera camera;
        camera.setPosition(0, 0, -400);
        camera.nearPlane = 1; camera.farPlane = 4096; camera.setFOV(65, width);
        AmbientLight ambient(Color{50, 65, 80});
        DirectionalLight light(Vector3{40, 45, 0}, Color{255, 245, 230}, 230);
        PointLight point(Vector3{-100,80,-80}, Color{255,100,50});point.rangeMax=700;
        std::unique_ptr<Object> cube(Primitives::createCube(170, 150, 130, &material));
        Scene scene(color.data() + guard, depth.data() + guard, width, height);
        scene.setCamera(&camera); scene.setAmbientLight(&ambient); scene.setDirectionalLight(&light);
        scene.setBackcolor(0x1234); scene.setClearBuffer(true); scene.addObject(cube.get());
        scene.addPointLight(&point);
        Sprite2D solid, sprite;
        solid.material = &overlay; solid.x = 3; solid.y = 2;
        solid.width = 15; solid.height = 8; solid.alpha = 135;
        sprite.material = &textured; sprite.x = 38; sprite.y = 30;
        sprite.scale = 3; sprite.alpha = 187;
        sprite.textureFlags = Sprite2D::MIRROR_X | Sprite2D::FLIP_Y;
        scene.addSprite(&solid); scene.addSprite(&sprite);
        ParticleSystem particles;particles.seed(123);particles.additiveSparks=true;
        LensFlare::Element element;element.material=&overlay;element.width=8;element.height=8;element.axisT=.5f;
        element.blendMode=BlendMode::BLEND_REPLACE;
        LensFlare flare(&scene,&element,1,0,Vec3f{.4f,.3f,1});
        struct ExtraContext {ParticleSystem *particles;LensFlare *flare;Camera *camera;};
        ExtraContext extra{&particles,&flare,&camera};
        scene.setPostGeometryHook([](Scene &s,void *user) {
            auto &e=*static_cast<ExtraContext *>(user);auto *r=s.getRenderer();int queries=r->pickQueryCount;r->pickQueryCount=0;
            e.particles->render(&s,e.camera,width,height);r->pickQueryCount=queries;e.flare->update(.05f,width,height);
        },&extra);
        for (int frame = 0; frame < 48; ++frame) {
            if(frame%8==0) {
                particles.emitSparks({0,0,-100},{0,1,0},200,8);
                particles.emitWaterSplash({0,0,-100},{0,0,1},{1,0,0},{0,1,0},200,8);
            }
            particles.update(.05f);flare.prepare(&camera,width,height);
            material.shadingMode = static_cast<ShadingMode>(frame % 7);
            material.alpha = frame % 3 ? 255 : 117;
            material.specularExponent = frame % 2 ? 0 : 32;
            material.diffuseMap = frame % 4 ? nullptr : &texture;
            if(frame%8==0) {
                unsigned filter=(frame/8)%3;tiledTexture.bilinear=filter!=0;
                tiledTexture.tiledFilter=filter==2 ? TileFilter::ThreePoint : TileFilter::Bilinear;
                material.diffuseMap=&tiledTexture;
            }
            cube->setRotation(frame * 7, frame * 13, frame * 3);
            material.shader=frame%7==4 ? &shader : nullptr;
            cube->setPosition(frame % 2 ? -10 : 5, 0, 0);
            scene.backgroundGradientColors = frame % 2 ? gradient.data() : nullptr;
            scene.getRenderer()->wireframeMode = frame % 12 == 11;
            scene.getRenderer()->textureLodEnabled=frame%3!=0;
            scene.getRenderer()->textureLodNear=200;scene.getRenderer()->textureLodFar=600;
            scene.getRenderer()->reflectBuffer = frame ? reflection.data() : nullptr;
            sprite.blendMode = frame % 2 ? BlendMode::BLEND_ADD : BlendMode::BLEND_REPLACE;
            scene.render();
            assert(scene.lastFrameRasterizedTriangles > 0);
            assert(std::all_of(color.begin(), color.begin() + guard, [](auto p) { return p == sentinel; }));
            assert(std::all_of(color.end() - guard, color.end(), [](auto p) { return p == sentinel; }));
            assert(std::all_of(depth.begin(), depth.begin() + guard, [](auto p) { return p == sentinel; }));
            assert(std::all_of(depth.end() - guard, depth.end(), [](auto p) { return p == sentinel; }));
            std::copy_n(color.data() + guard, count, reflection.data());
            assert(std::fwrite(color.data() + guard, sizeof(uint16_t), count, output) == count);
        }
        assert(shader.calls>0);
    }
    // Independent constant-color check verifies channel order and depth writes.
    std::vector<uint16_t> color(count), depth(count, 0xffff);
    Camera camera; camera.nearPlane = 1; camera.farPlane = 4096;
    Rasterizer raster(color.data(), width, height, depth.data(), &camera);
    Material red(uint16_t(0xf800)); red.shadingMode = ShadingMode::UNLIT;
    RenderVertex a, b, c;
    a.position = {-10, -10, 100}; b.position = {100, -10, 100}; c.position = {-10, 100, 100};
    assert(raster.drawTriangle(a, b, c, &red, nullptr, nullptr, false, false, false, 0));
    assert(depth[10 * width + 10] == 100);
    assert(std::fwrite(color.data(), sizeof(uint16_t), count, output) == count);
    struct ScreenShader:Shader {
        unsigned calls=0;
        uint16_t getPixel(uint32_t u,uint32_t v,ShaderParameters *p) override {
            assert(p&&int32_t(u)==p->screenPos.x&&int32_t(v)==p->screenPos.y);++calls;return 0x07e0;
        }
    } screen;
    screen.screenSpace=true;screen.hasAlpha=true;screen.alphaColor=0x07e0;
    std::fill(color.begin(),color.end(),0);std::fill(depth.begin(),depth.end(),0xffff);
    red.shader=&screen;
    assert(raster.drawTriangle(a,b,c,&red,nullptr,nullptr,false,false,false,0)&&screen.calls>0);
    assert(std::all_of(color.begin(),color.end(),[](auto v) {return v==0;}));
    assert(std::all_of(depth.begin(),depth.end(),[](auto v) {return v==0xffff;}));
    screen.hasAlpha=false;
    assert(raster.drawTriangle(a,b,c,&red,nullptr,nullptr,false,false,false,0));
    assert(framebufferDecode(color[10*width+10])==0x07e0&&depth[10*width+10]==100);
    std::fclose(output);
}
