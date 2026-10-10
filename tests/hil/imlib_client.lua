local image, vision = solaros.image, solaros.vision
local source = image.open('/dl/.imlib-scene.png')
local color = image.open('/dl/.imlib-color.png')
local owned = {source, color}
local ok, error_value = pcall(function()
    local s = vision.statistics(source, {x=8,y=6,width=8,height=6})
    assert(s.channels.gray.mean == 200)
    local h = vision.histogram(source)
    assert(#h.channels.gray == 256)
    local b = vision.blobs(color, {format='rgb565',thresholds={{20,80,30,127,0,127}}})
    assert(#b.blobs == 1 and b.blobs[1].pixels == 48 and b.blobs[1].x == 8)
    local mask = vision.binary(source, {thresholds={{128,255}}})
    owned[#owned+1] = mask
    local r = vision.process(mask, 'opening')
    owned[#owned+1] = r.image
    local clean = vision.blobs(r.image, {thresholds={{128,255}}})
    assert(#clean.blobs == 1 and clean.blobs[1].pixels == 48)
    assert(not pcall(vision.statistics, source, {bins=1}))
    assert(not pcall(vision.mean, source, {ksize=4}))
    print('IMLIB_LUA_OK', #b.blobs, r.process_us, r.workspace_peak_bytes)
end)
for i=#owned,1,-1 do image.close(owned[i]) end
if not ok then error(error_value) end
