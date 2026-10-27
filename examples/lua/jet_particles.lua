-- Seeded sparks and a light flare over a rotating cube.
local solaros = require("solaros")
local scene <close> = solaros.jet.open(nil)
scene:gradient{0x102040, 0xffffff}
local cube = scene:cube(100, 100, 100, 0x2192ff)
local pool = scene:particles{seed=123, additive=true}
local pixels = {}
for y = 0, 15 do
    for x = 0, 15 do
        local value = math.max(0, 255-math.floor(((x-7.5)^2+(y-7.5)^2)*5))
        pixels[#pixels+1] = string.char(value, value, value)
    end
end
local texture = scene:texture(16, 16, table.concat(pixels), {format="rgb888", alpha_key=0})
local glow = scene:material{texture=texture, shading="unlit"}
local flare = scene:flare({
    {material=glow, width=16, height=16, scale=2},
    {material=glow, width=16, height=16, axis=0.6}
}, {x=-0.35, y=0.2, z=1})
for frame = 0, 359 do
    if solaros.should_exit() then break end
    if frame % 12 == 0 then scene:particles_emit(pool, {z=-80, count=24, speed=200}) end
    scene:step(1/30)
    scene:object_set(cube, {rx=frame % 360, ry=(frame*2) % 360})
    scene:render()
    solaros.time.sleep_ms(1)
end
