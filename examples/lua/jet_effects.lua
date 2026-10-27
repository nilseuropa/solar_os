-- Cycle through the optional filters on a rotating cube.
local solaros = require("solaros")
local presets = {{}, {cel_bits=6}, {fxaa=true}, {bloom=true},
                 {crt=96}, {motion_blur=70}, {chromatic=3}, {pixelate=4}}
local scene <close> = solaros.jet.open(nil)
scene:background(0x081020)
local material = scene:material{color=0xa0ffff, shading="phong", specular=100, gloss=32}
local cube = scene:primitive{kind="cube", width=180, height=180, depth=180, material=material}
for frame = 0, 359 do
    if solaros.should_exit() then break end
    if frame % 45 == 0 then
        scene:postfx_clear()
        scene:postfx(presets[(frame // 45) % #presets+1])
    end
    scene:object_set(cube, {rx=frame % 360, ry=(frame*2) % 360})
    scene:render()
    solaros.time.sleep_ms(1)
end
