# Upload to SolarOS and pass the MobileNetV2 model and a square PNG sample.
import binascii
import gc
import hashlib
import json
import math
import sys
from solaros import inference, image, time


def fails(fn, *args):
    try:
        fn(*args)
    except (OSError, ValueError, TypeError):
        return
    raise AssertionError("expected rejection")


def checksum(data):
    return binascii.hexlify(hashlib.sha256(data).digest()).decode()


mean, std = [123.675,116.28,103.53], [58.395,57.12,57.375]
model = inference.load(sys.argv[1])
picture = None
try:
    assert inference.info(model)["backend_version"] == "3.3.13"
    picture = image.open(sys.argv[2])
    width, height = image.size(picture)
    assert width == height and width > 1
    options = {"layout":"NHWC","color":"RGB","resize":"stretch","mean":mean,"std":std}
    reference = image.to_rgb(picture,224,224)
    tables = []
    for m, s in zip(mean,std):
        table = bytearray(256)
        for v in range(256):
            table[v] = max(-128,min(127,int(math.floor((v-m)/s*64+.5)))) & 255
        tables.append(table)
    begin = time.uptime_ms()
    for i in range(0,len(reference),3):
        for c in range(3):
            reference[i+c] = tables[c][reference[i+c]]
        if i % (224*3*8) == 0:
            time.sleep_ms(0)
    python_ms = time.uptime_ms()-begin
    prepared = inference.prepare_image(model,"input.1",picture,options)
    assert prepared["data"] == reference
    tensor_hash = checksum(reference)
    first_pixel = reference[:3]
    native_us = prepared["preprocess_us"]
    del prepared, reference
    gc.collect()
    fails(inference.prepare_image,model,"missing",picture)
    fails(inference.prepare_image,model,"input.1",picture,{"std":[0]})
    fails(inference.prepare_image,model,"input.1",picture,{"color":"RGB\x00suffix"})
    fails(inference.prepare_image,model,"input.1",picture,{"mean":[1,2]})
    fails(inference.prepare_image,model,"input.1",picture,{"x":width})
    fails(inference.prepare_image,model,"input.1",picture,{"pad":[256]})
    fails(inference.prepare_image,model,"input.1",picture,{"extra":1})
    options.update(resize="letterbox",height=height//2)
    prepared = inference.prepare_image(model,"input.1",picture,options)
    transform = prepared["transform"]
    assert transform["resized_width"] == 224 and transform["resized_height"] == 112 and transform["pad_top"] == 56
    padding = bytes(tables[c][0] for c in range(3))
    assert prepared["data"][:3] == padding and prepared["data"][-3:] == padding
    assert prepared["data"][56*224*3:56*224*3+3] == first_pixel
    retained = prepared["data"]
finally:
    if picture is not None:
        image.close(picture)
    inference.close(model)
assert len(retained) == 150528
print(json.dumps({"python_quantize_ms":python_ms,"native_fused_us":native_us,"tensor_sha256":tensor_hash}))
print("IMAGE_TENSOR_PYTHON_OK")
