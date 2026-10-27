"""Seeded sparks and a light flare over a rotating cube."""
import sys
import solaros

target = sys.argv[1] if len(sys.argv) > 1 else None
with solaros.jet.open(target) as scene:
    scene.gradient((0x102040, 0xffffff))
    cube = scene.cube(100, 100, 100, color=0x2192ff)
    pool = scene.particles(seed=123, additive=True)
    pixels = bytearray()
    for y in range(16):
        for x in range(16):
            value = max(0, 255 - int(((x - 7.5) ** 2 + (y - 7.5) ** 2) * 5))
            pixels.extend((value, value, value))
    texture = scene.texture(16, 16, pixels, format="rgb888", alpha_key=0)
    glow = scene.material(texture=texture, shading="unlit")
    flare = scene.flare(({"material": glow, "width": 16, "height": 16, "scale": 2},
                         {"material": glow, "width": 16, "height": 16, "axis": 0.6}),
                        x=-0.35, y=0.2, z=1)
    for frame in range(360):
        if solaros.should_exit():
            break
        if frame % 12 == 0:
            scene.particles_emit(pool, z=-80, count=24, speed=200)
        scene.step(1 / 30)
        scene.object_set(cube, rx=frame % 360, ry=(frame * 2) % 360)
        scene.render()
        solaros.time.sleep_ms(1)
