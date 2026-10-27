-- Cycle full textures, texture-distance fading and scene wireframe.
local solaros = require("solaros")
local scene <close> = solaros.jet.open(nil)
local texture = scene:texture(2,2,"\248\000\007\224\000\031\255\255")
local material = scene:material{texture=texture,color=0x247de0,shading="unlit"}
local cube = scene:cube(150,150,150)
scene:assign(cube,material)
for frame=0,179 do
    if solaros.should_exit() then break end
    local phase=math.floor(frame/60)
    scene:settings{texture_lod=phase==1,texture_near=350,texture_far=650,wireframe=phase==2}
    scene:transform(cube,0,0,0,20,frame*3,0)
    scene:render()
    solaros.time.sleep_ms(10)
end
