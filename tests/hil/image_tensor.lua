-- Copy the classifier and square PNG sample to /dl before this check.
local inf, image = solaros.inference, solaros.image
local model = inf.load('/dl/models/imagenet/imagenet_cls_mobilenetv2_s8_v1.espdl')
local picture = image.open('/dl/cat.png')
local mean, std = {123.675,116.28,103.53}, {58.395,57.12,57.375}
local retained
local ok, err = pcall(function()
 local rgb = image.to_rgb(picture,224,224)
 collectgarbage()
 local prepared = inf.prepare_image(model,'input.1',picture,
   {layout='NHWC',color='RGB',resize='stretch',mean=mean,std=std})
 assert(#prepared.data == #rgb and prepared.transform.input_width == 224)
 for i=1,#rgb do
  local c=(i-1)%3+1
  local value=math.max(-128,math.min(127,math.floor((string.byte(rgb,i)-mean[c])/std[c]*64+.5)))
  assert(string.byte(prepared.data,i)==(value&255))
  if i%8192==0 then solaros.time.sleep_ms(0) end
 end
 assert(not pcall(inf.prepare_image,model,'input.1',picture,{std={0}}))
 assert(not pcall(inf.prepare_image,model,'input.1',picture,{color='RGB\0suffix'}))
 assert(not pcall(inf.prepare_image,model,'input.1',picture,{extra=1}))
 retained = prepared.data
 print('NATIVE_PREPROCESS_US',prepared.preprocess_us)
end)
image.close(picture); inf.close(model)
assert(ok,err); assert(#retained==150528)
print('IMAGE_TENSOR_LUA_OK')
