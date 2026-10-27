-- Install the jet library first. For a port shell, replace nil below with
-- a ready target from `display list`. Ctrl+] stops the script.
local solaros = require("solaros")
local scene <close> = solaros.jet.open(nil)
scene:background(0xffffff)
scene:camera(0, 0, -500, 0, 0, 0, 60)
scene:light(0x5a5a5a, 0xfff5e6, 220, 35, 40)
local blue = scene:cube(150, 150, 150, 0x2192ff)
local orange = scene:cube(80, 80, 80, 0xff7d00)
for frame = 0, 719 do
    if solaros.should_exit() then break end
    scene:transform(blue, -55, 0, 100, frame % 360, (frame * 2) % 360, 15)
    scene:transform(orange, 100, 35, 30, 25, (-frame) % 360, frame % 360)
    scene:render()
    solaros.time.sleep_ms(1)
end
