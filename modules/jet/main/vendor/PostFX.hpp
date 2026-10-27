#ifndef POSTFX_HPP
#define POSTFX_HPP

#include <cstdint>
#include <cstddef>
#include <memory>
#include "JetConfig.hpp"
#ifndef JET_RUNTIME_POSTFX
#define JET_RUNTIME_POSTFX 0
#endif

namespace Renderer {

/// @brief Collection of in-place post-processing effects on an RGB565 framebuffer.
///
/// Effects compile out unless their POSTFX_* flag or JET_RUNTIME_POSTFX is
/// enabled. Runtime working buffers are prepared once and reused every frame.
class PostFX {
public:
    /// @brief Construct a PostFX instance sized for a specific framebuffer.
    /// @param screenWidth Framebuffer width in pixels.
    /// @param screenHeight Framebuffer height in pixels.
    PostFX(int screenWidth, int screenHeight);
    ~PostFX();
    enum Buffers : unsigned {FXAA=1,BLOOM=2,MOTION=4,CHROMATIC=8};
    void prepare(unsigned buffers);
    size_t scratchBytes() const;
    void resetHistory() {motionReady=false;}
    bool hasHistory() const {return motionReady;}

    /// @brief Apply a 5-tap luminance-driven anti-aliasing pass in place.
    /// @param framebuffer RGB565 framebuffer of size screenWidth*screenHeight.
    void applyFXAA(uint16_t* framebuffer);

    /// @brief Apply a separable box-blur bloom pass in place.
    /// @param framebuffer RGB565 framebuffer of size screenWidth*screenHeight.
    void applyBloom(uint16_t* framebuffer);

    /// @brief Apply CRT with the configured intensity and buffer layout.
    /// Full-height buffers process all odd rows. Packed fields default to the
    /// even physical field (unchanged); use the overload to select odd rows.
    /// @param framebuffer RGB565 colour storage for the configured layout.
    void applyCRT(uint16_t* framebuffer);

    /// @brief Darken odd physical display rows in place, with no scratch buffer.
    /// Honours HALF_WIDTH_BUFFERS and FIELD_BUFFERS. Packed fields always
    /// contain alternate physical rows; oddRows selects their display parity.
    /// With full-height buffers, interlaced=true touches only the selected
    /// parity, preventing repeated darkening of retained rows.
    /// intensity is 0 (unchanged) to 255 (black), scaling RGB equally.
    void applyCRT(uint16_t* framebuffer, uint8_t intensity,
                  bool interlaced, bool oddRows);

    /// @brief Blend the current frame with the previous frame to produce motion blur.
    /// @param framebuffer RGB565 framebuffer of size screenWidth*screenHeight.
    void applyMotionBlur(uint16_t* framebuffer, unsigned strength=MOTION_BLUR_STRENGTH);

    /// @brief Apply a chromatic aberration channel-shift effect in place.
    /// @param framebuffer RGB565 framebuffer of size screenWidth*screenHeight.
    void applyChromatic(uint16_t* framebuffer, unsigned offset=CHROMATIC_OFFSET);

    /// @brief Quantize the framebuffer to PIXELATE_SIZE x PIXELATE_SIZE blocks in place.
    /// @param framebuffer RGB565 framebuffer of size screenWidth*screenHeight.
    void applyPixelate(uint16_t* framebuffer, unsigned size=PIXELATE_SIZE);

private:
    int screenWidth;
    int screenHeight;
    bool motionReady=false;
    #if POSTFX_BLOOM || JET_RUNTIME_POSTFX
    std::unique_ptr<uint16_t[]> bloomBuffer,bloomTemp;
    #endif
    #if POSTFX_MOTION_BLUR || JET_RUNTIME_POSTFX
    std::unique_ptr<uint16_t[]> previousFrame;
    #endif
    #if POSTFX_ANTIALIASING || JET_RUNTIME_POSTFX
    // Cached unpacked channels + luminance for the previous (top) row.
    // Layout: 4 * screenWidth int16s, indexed [x*4 + {0=r,1=g,2=b,3=lum}].
    std::unique_ptr<int16_t[]> fxaaTopRow;
    #endif
    #if POSTFX_CHROMATIC || JET_RUNTIME_POSTFX
    std::unique_ptr<uint16_t[]> chromaticRow;
    #endif
};

} // namespace Renderer

#endif // POSTFX_HPP
