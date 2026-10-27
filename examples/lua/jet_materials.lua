-- A textured cube with a reusable, animated Phong material.
local solaros = require("solaros")
local scene <close> = solaros.jet.open(nil)
local pixels = ""
for y = 0, 3 do
    for x = 0, 3 do pixels = pixels .. string.char((x + y) % 2) end
end
local texture = scene:texture(4, 4, pixels, {
    format="index8", palette={0x2192ff, 0xffffff}, name="checker"
})
local material = scene:material{
    texture=texture, shading="phong", specular=80, gloss=32, name="glossy"
}
local cube = scene:cube(180, 180, 180)
scene:assign(cube, material)
for frame = 0, 359 do
    if solaros.should_exit() then break end
    scene:texture_set(texture, {offset=(frame // 30) % 2})
    scene:transform(cube, 0, 0, 100, frame % 360, (frame * 2) % 360, 0)
    scene:render()
    solaros.time.sleep_ms(1)
end
