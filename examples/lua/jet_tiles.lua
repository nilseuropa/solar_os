-- Cycle tiled palette filters on a textured cube.
local solaros = require("solaros")
local scene <close> = solaros.jet.open(nil)
local pixels = {}
for y = 0, 31 do
    for x = 0, 31 do pixels[#pixels+1] = string.char((x//4+y//4) % 2) end
end
local texture = scene:texture(32,32,table.concat(pixels), {
    format="index8", palette={0x2192ff,0xffffff}, tiled=true, address="clamp"
})
local material = scene:material{texture=texture, shading="gouraud"}
local cube = scene:primitive{kind="cube", width=180, height=180, depth=180, material=material}
local filters = {"nearest", "bilinear", "three_point"}
for frame = 0, 359 do
    if solaros.should_exit() then break end
    if frame % 120 == 0 then scene:texture_set(texture, {filter=filters[(frame//120) % #filters+1]}) end
    scene:object_set(cube, {rx=frame % 360, ry=(frame*2) % 360})
    scene:render()
    solaros.time.sleep_ms(1)
end
