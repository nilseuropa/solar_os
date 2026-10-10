-- Inspect every OS-owned pipeline after its creating interpreter has exited.
local pipeline = solaros.pipeline
for _, p in ipairs(pipeline.list()) do
    local r = pipeline.result(p)
    local s = pipeline.status(p)
    assert(r and r.pipeline == p and r.sequence <= s.sequence)
    if s.done then assert(pipeline.result(p, s.sequence) == nil) end
    if r.result.kind == 'qr' then
        assert(r.result.codes[1].payload_hex == '536f6c61724f532051520062696e617279ff')
    elseif r.result.result.kind == 'classification' and s.source:match('/dl/cat%.png$') then
        assert(r.result.result.classes[1].label == 'tabby')
    elseif r.result.result.kind == 'detection' and s.source:match('/dl/pedestrian%.png$') then
        assert(#r.result.result.detections == 3)
    end
    print('PIPELINE_LUA_OK', p, s.frames, r.result.kind)
end
