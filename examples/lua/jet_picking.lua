-- A screen-space cursor picks a rotating cube beneath the HUD.
local solaros = require("solaros")
local scene <close> = solaros.jet.open(nil)
local status = scene:status()
local x, y = status.width // 2, status.height // 2
local cube = scene:cube(180, 180, 180, 0x2192ff)
local cursor = scene:sprite{x=x-2, y=y-2, width=5, height=5, color=0xff0000}
scene:pick{{x, y}}
for frame = 0, 359 do
    if solaros.should_exit() then break end
    scene:object_set(cube, {rx=frame % 360, ry=(frame * 2) % 360})
    scene:render()
    local hit = scene:picks()[1].hit
    scene:sprite_set(cursor, {color=hit and 0x20ff20 or 0xff0000})
    solaros.time.sleep_ms(1)
end
