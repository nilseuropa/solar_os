-- Six independently rotating cubes share one immutable mesh.
local solaros = require("solaros")
local scene <close> = solaros.jet.open(nil)
local source = scene:cube(70, 70, 70)
local prototype = scene:prototype(source, {name="cube"})
scene:remove(source)
local colors = {scene:material{color=0x2192ff}, scene:material{color=0xff9020}}
local batch = scene:instance(prototype, {x=-120, y=-65, material=colors[1]})
for index = 1, 5 do
    scene:instance_add(batch, prototype, {
        x=(index % 3-1)*120, y=index < 3 and -65 or 65, material=colors[index % 2+1]
    })
end
for frame = 0, 359 do
    if solaros.should_exit() then break end
    for index = 0, 5 do
        scene:instance_set(batch, index+1, {
            rx=(frame+index*20) % 360, ry=(frame*2+index*30) % 360
        })
    end
    scene:render()
    solaros.time.sleep_ms(1)
end
