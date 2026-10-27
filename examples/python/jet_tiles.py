"""Cycle tiled palette filters on a textured cube."""
import sys
import solaros

target = sys.argv[1] if len(sys.argv) > 1 else None
with solaros.jet.open(target) as scene:
    pixels = bytes((x // 4 + y // 4) % 2 for y in range(32) for x in range(32))
    texture = scene.texture(32, 32, pixels, format="index8", palette=(0x2192ff, 0xffffff),
                            tiled=True, address="clamp")
    material = scene.material(texture=texture, shading="gouraud")
    cube = scene.primitive(kind="cube", width=180, height=180, depth=180, material=material)
    filters = ("nearest", "bilinear", "three_point")
    for frame in range(360):
        if solaros.should_exit():
            break
        if frame % 120 == 0:
            scene.texture_set(texture, filter=filters[(frame // 120) % len(filters)])
        scene.object_set(cube, rx=frame % 360, ry=(frame * 2) % 360)
        scene.render()
        solaros.time.sleep_ms(1)
