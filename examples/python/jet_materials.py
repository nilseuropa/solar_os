"""A textured cube with a reusable, animated Phong material."""
import sys
import solaros

target = sys.argv[1] if len(sys.argv) > 1 else None
with solaros.jet.open(target) as scene:
    pixels = bytes((x + y) % 2 for y in range(4) for x in range(4))
    texture = scene.texture(4, 4, pixels, format="index8",
                            palette=(0x2192ff, 0xffffff), name="checker")
    material = scene.material(texture=texture, shading="phong", specular=80,
                              gloss=32, name="glossy")
    cube = scene.cube(180, 180, 180)
    scene.assign(cube, material)
    for frame in range(360):
        if solaros.should_exit():
            break
        scene.texture_set(texture, offset=(frame // 30) % 2)
        scene.transform(cube, z=100, rx=frame % 360, ry=(frame * 2) % 360)
        scene.render()
        solaros.time.sleep_ms(1)
