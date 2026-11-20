-- Online categorical prediction and byte-image encoding; install aogmaneo first.
local a = solaros.aogmaneo
local model = a.create({{size={1,1,4}}}, {{size={2,2,16}}}, 12345)
local ok, err = pcall(function()
    for tick=0,199 do a.step(model, {{tick % 4}}) end
    print("Predicted category:", a.prediction(model)[1])
    print("Hidden codes:", table.concat(a.hidden(model), ","))
end)
a.destroy(model)
if not ok then error(err) end

local image = a.image_create({2,2,8}, {{size={4,4,1},radius=1}})
ok, err = pcall(function()
    local pixels = string.rep(string.char(0,64,128,255), 4)
    local codes
    for tick=1,20 do codes=a.image_step(image, {pixels}, true, true) end
    local reconstructed = a.image_reconstruct(image, codes)
    print("Image codes:", table.concat(codes, ","))
    print("Reconstructed bytes:", #reconstructed)
end)
a.destroy(image)
if not ok then error(err) end
