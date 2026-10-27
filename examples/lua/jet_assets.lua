-- Import an OBJ cube, MTL material, and a tiny BMP from memory.
local solaros = require("solaros")
local obj = [[v -1 -1 -1
v 1 -1 -1
v 1 1 -1
v -1 1 -1
v -1 -1 1
v 1 -1 1
v 1 1 1
v -1 1 1
vt 0 0
vt 1 0
vt 1 1
vt 0 1
usemtl checker
f 1/1 4/4 3/3 2/2
f 5/1 6/2 7/3 8/4
f 1/1 5/2 8/3 4/4
f 2/1 3/4 7/3 6/2
f 4/1 8/2 7/3 3/4
f 1/1 2/2 6/3 5/4
]]
local mtl = "newmtl checker\nKd 1 1 1\nillum 1\nmap_Kd checker.bmp\n"
local pixels = string.char(255,0,0,255,255,255,0,0,0,0,255,0,255,0,0,0)
local bmp = string.pack("<c2I4I2I2I4I4I4I4I2I2I4I4I4I4I4I4",
                        "BM",70,0,0,54,40,2,2,1,24,0,16,0,0,0,0) .. pixels
local scene <close> = solaros.jet.open(nil)
local texture = scene:load_bmp(bmp, {name="checker.bmp", address="clamp"})
local materials = scene:load_mtl(mtl)
local cube = scene:load_obj(obj, {scale=0.7, name="imported cube"})
for frame = 0, 359 do
    if solaros.should_exit() then break end
    scene:object_set(cube, {rx=frame % 360, ry=(frame * 2) % 360})
    scene:render()
    solaros.time.sleep_ms(1)
end
