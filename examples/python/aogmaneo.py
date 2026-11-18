"""Online categorical prediction and byte-image encoding; install aogmaneo first."""
import solaros

a = solaros.aogmaneo
model = a.create([{"size": (1, 1, 4)}], [{"size": (2, 2, 16)}], seed=12345)
try:
    for tick in range(200):
        a.step(model, [[tick % 4]])
    print("Predicted category:", a.prediction(model)[0])
    print("Hidden codes:", a.hidden(model))
finally:
    a.destroy(model)

image = a.image_create((2, 2, 8), [{"size": (4, 4, 1), "radius": 1}])
try:
    pixels = bytes([0, 64, 128, 255] * 4)
    for tick in range(20):
        codes = a.image_step(image, [pixels], learn=True, learn_reconstruction=True)
    reconstructed = a.image_reconstruct(image, codes)
    print("Image codes:", codes)
    print("Reconstructed bytes:", len(reconstructed))
finally:
    a.destroy(image)
