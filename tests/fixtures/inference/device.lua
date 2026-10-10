-- Upload alongside fixtures to /inference-hil; run with agent script lua.
local inference = solaros.inference
local a = {1,-2,3,4,-5,6,7,-8}
local b = {2,3,-4,1,6,-2,0,4}
local function fails(fn, ...) assert(not pcall(fn, ...)) end
assert(#inference.list()==0,"run this fixture with no pre-existing resident models")
print("LUA_INFERENCE_START")
fails(inference.load,"/inference-hil/arithmetic_oversized.espdl")
print("OVERSIZED_LOAD_REJECTED")
local function check(bytes, fmt, subtract)
    local values = {string.unpack(fmt, bytes)}
    for i=1,#values-1 do
        local index = (i-1)%8+1
        assert(values[i] == (subtract and a[index]-b[index] or a[index]+b[index]))
    end
end
for _, model in ipairs({{"arithmetic_int8","int8","bbbbbbbb",4},
                        {"arithmetic_float32","float32","<ffffffff",4},
                        {"arithmetic_simd_int8","int8",string.rep("b",32),16}}) do
    local name, dtype, fmt, width = table.unpack(model)
    local path = "/inference-hil/" .. name .. ".espdl"
    local handle = inference.load(path)
    local info = inference.info(handle)
    assert(info.backend == "espdl" and info.inputs.a.shape[1] == 2)
    assert(inference.outputs(handle).sum.dtype == dtype)
    local av = string.pack("bbbbbbbb",table.unpack(a)):rep(width//4)
    local bv = string.pack("bbbbbbbb",table.unpack(b)):rep(width//4)
    if dtype == "float32" then
        av, bv = string.pack(fmt,table.unpack(a)), string.pack(fmt,table.unpack(b))
    end
    local typed = {data=av,dtype=dtype,shape={2,width}}
    local r
    for i=1,75 do
        r = inference.run(handle,{b=bv,a=(i%2==0 and av or typed)})
        check(r.outputs.sum.data,fmt,false); check(r.outputs.difference.data,fmt,true)
        if i%10==0 then collectgarbage() end
    end
    print("TIMING",name,r.input_us,r.inference_us,r.output_us,r.elapsed_us)
    fails(inference.run,handle,{a=av}); fails(inference.run,handle,{a=av,b=bv:sub(2)})
    fails(inference.run,handle,{["a\0suffix"]=av,b=bv})
    typed.dtype="uint8"; fails(inference.run,handle,{a=typed,b=bv}); typed.dtype=dtype
    typed.dtype=dtype .. "\0suffix"; fails(inference.run,handle,{a=typed,b=bv}); typed.dtype=dtype
    typed.shape={1,8}; fails(inference.run,handle,{a=typed,b=bv})
    fails(inference.run,handle,{a=av,b=bv},1)
    -- Recovery must also fit below the former 100 ms worker-reap delay.
    local recovered = inference.run(handle,{a=av,b=bv},50)
    check(recovered.outputs.sum.data,fmt,false)
    fails(inference.load,"/inference-hil/missing.espdl")
    fails(inference.load,"/inference-hil/corrupt.espdl")
    local retained = r.outputs.sum.data
    inference.reset(handle); inference.close(handle)
    check(retained,fmt,false); fails(inference.info,handle)
    local again = inference.load(path); assert(again ~= handle)
    inference.close_all(); fails(inference.info,again)
    for i=1,4 do inference.load(path) end
    fails(inference.load,path); inference.close_all()
    print("LUA_MODEL_OK",name)
end
for i=1,4 do inference.load("/inference-hil/arithmetic_int8.espdl") end
print("RESIDENT_HANDLES",table.unpack(inference.list()))
print("LUA_INFERENCE_OK") -- These four models remain resident; explicitly unload them before another fixture run.
