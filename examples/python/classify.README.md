# ImageNet picture classification

Put `classify.py`, `labels.txt`, and
`imagenet_cls_mobilenetv2_s8_v1.espdl` together in `/dl` on mounted storage.
Use the ESP32-S3 model and the ordered ImageNet labels from
[ESP-DL v3.3.13](https://github.com/espressif/esp-dl/tree/v3.3.13/models/imagenet_cls).
The model has 1,000 classes, a 224×224 RGB int8 input, and quantized logits.
It classifies the whole picture; it does not locate objects.

```text
python /dl/classify.py /dl/cat.bmp
python /dl/classify.py /pictures/photo.bmp --repeat 5 --warmup 1
python /dl/classify.py /pictures/first.bmp /pictures/second.bmp --top 3
python /dl/classify.py /pictures/photo.bmp --json
python /dl/classify.py /pictures/photo.png --mode dual --repeat 5
```

On firmware with `solaros.image.to_rgb`, JPEG, PNG, GIF, and WebP pictures work
through the native decoder:

```text
python /dl/classify.py /pictures/photo.jpg
```

The script resizes to 224×224 with nearest-neighbor sampling, preserving the
upstream adapter's coordinate mapping. It uses RGB means
`[123.675, 116.28, 103.53]` and standard deviations
`[58.395, 57.12, 57.375]`, then quantizes with the exported input exponent,
half-up rounding, saturation, and zero point zero. Firmware with
`inference.prepare_image` fuses resize, normalization, and quantization natively
for native decoded images. BMP/PPM and older firmware use the portable Python
path, quantizing the RGB bytearray in place. Output uses the exported exponent and stable softmax over
all 1,000 classes, followed by ranked labels. Scores are model softmax scores.

The model loads once per script invocation, stays resident across all pictures
and repeats, and closes in `finally`, including errors. Use multiple picture
arguments to amortize model loading. `--model PATH` and `--labels PATH` override
the files; the script checks the expected tensor contract and label count.

`--mode single|auto|dual` selects execution mode on firmware that provides
`inference.set_mode`; the default is `single`. `auto` uses ESP-DL's operator
heuristics, while `dual` splits supported operators between both cores. Mode
selection affects model execution independently of preprocessing.

Statistics report model-load wall time, observed resident SRAM/PSRAM deltas,
picture read/decode/resize time, normalization/quantization time, inference-call
wall time, native input-copy/execution/output-copy time, softmax/top-k time,
batch wall time, and Python heap free bytes before/after each picture. Native
execution includes cooperative operator yields. Repeated inference statistics
exclude warm-ups, explicit pre-run garbage collection, and preprocessing.
Batch wall time includes warm-ups, repeats, and garbage collection. Console
printing is outside these measurements. Heap deltas are not peak requirements.
`preprocess_backend` identifies the native/Python path and `native_preprocess_ms`
reports the fused native time when available. Legacy `decode_resize_ms` contains
read/decode time on the fused path; `normalize_quantize_ms` includes its resize.

Pictures are limited to 2 million pixels and must fit available native memory
alongside the resident model. RGB exports and script objects must also fit the
512 KiB Python heap. The classifier never draws on or claims a display.
Downsize large phone/camera pictures before copying them; 512×512 or smaller
is a useful starting point for this model on an 8 MiB PSRAM device.

## Model provenance

- ESP-DL tag: `v3.3.13`, upstream commit
  `6d3c4da1087d35916858dd0f9b3e9d6a85186e11`.
- Model: `models/imagenet_cls/models/s3/imagenet_cls_mobilenetv2_s8_v1.espdl`.
- Model bytes: `3612288`.
- SHA-256: `4945668ccd054baa6f52f541385dd59336c85f3b80bcae127cce41bab2e18871`.
- Labels: `esp-dl/vision/classification/imagenet_category_name.hpp`, in order.
- The model is stored on the device, not embedded in the firmware or committed
  in this repository. Preserve the upstream MIT license with deployed assets.
