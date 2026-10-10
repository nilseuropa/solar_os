-- Run between the Python fixture's load and unload invocations.
-- lua /dl/inference_residency.lua
local inference = solaros.inference
local handle = inference.find('/dl/models/arithmetic/bundle.json')
assert(handle and inference.info(handle).bundle)
local result = inference.run_bundle(handle, {
    a=string.char(1,254,3,4,251,6,7,248),
    b=string.char(2,3,252,1,6,254,0,4),
})
assert(result.outputs.sum.data==string.char(3,1,255,5,1,4,7,252))
assert(result.result.kind=='raw')
inference.set_mode(handle,'auto')
assert(inference.info(handle).mode=='auto')
print('INFERENCE_RESIDENCY_LUA_OK',handle)
