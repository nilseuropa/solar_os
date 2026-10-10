from solaros import image, vision

picture = image.open("/dl/.imlib-large.png")
try:
    print("IMLIB_CANCEL_READY")
    while True:
        result = vision.process(picture, "gaussian", {"ksize": 3, "timeout_ms": 60000})
        image.close(result["image"])
        print("IMLIB_CANCEL_FRAME", result["process_us"])
finally:
    image.close(picture)
