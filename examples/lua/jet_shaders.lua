-- Native procedural shaders; optional protected Lua pixel callback.
local solaros = require("solaros")
local scene = solaros.jet.open(nil)
local ok, err = pcall(function()
    local shader = scene:shader(nil, {kind="checker", color=0x247de0, secondary=0xffa030, scale=6})
    local material = scene:material{shader=shader, shading="unlit"}
    local cube = scene:cube(140,140,140)
    scene:assign(cube,material)
    for frame=0,119 do
        if solaros.should_exit() then break end
        scene:transform(cube,0,0,0,20,frame*3,0)
        scene:render()
        solaros.time.sleep_ms(10)
    end
    -- Lua runs once per visible fragment. Keep callback surfaces small.
    scene:shader_set(shader,function(u,v,params)
        return math.floor(u*8)%2==0 and 0xffa030 or 0x247de0
    end)
    scene:object_set(cube,{scale_x=0.25,scale_y=0.25,scale_z=0.25})
    for frame=0,11 do
        if solaros.should_exit() then break end
        scene:transform(cube,0,0,0,20,frame*15,0)
        scene:render()
    end
end)
scene:close()
if not ok then error(err) end
