"""Generate our own QR fixtures: pip install qrcode pillow, then run this file.

The checked-in fixtures require no Python image libraries during test runs.
"""
from pathlib import Path
import qrcode
from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parent
PAYLOAD = b"SolarOS QR\x00binary\xff"
qr = qrcode.QRCode(version=2, error_correction=qrcode.constants.ERROR_CORRECT_M,
                   box_size=6, border=4)
qr.add_data(PAYLOAD, optimize=0)
qr.make(fit=False)
image = qr.make_image().convert("RGB")
image.save(ROOT / "qr.png")
image.save(ROOT / "qr.jpg", quality=90)
ImageOps.mirror(image).save(ROOT / "mirrored.png")
multiple = Image.new("RGB", (image.width * 2 + 20, image.height), "white")
multiple.paste(image, (0, 0))
multiple.paste(image, (image.width + 20, 0))
multiple.save(ROOT / "multiple.png")
many = Image.new("RGB", (495, 198), "white")
small = image.resize((99, 99), Image.Resampling.NEAREST)
for row in range(2):
    for col in range(5):
        many.paste(small, (col * 99, row * 99))
many.save(ROOT / "many.png")
Image.new("RGB", image.size, "white").save(ROOT / "blank.png")
corrupt = image.copy()
for y in range(85, 145):
    for x in range(85, 145):
        corrupt.putpixel((x, y), (255, 255, 255))
corrupt.save(ROOT / "corrupt.png")
