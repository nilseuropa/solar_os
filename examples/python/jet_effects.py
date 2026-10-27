"""Cycle through the optional filters on a rotating cube."""
import sys
import solaros

presets = ({}, {"cel_bits": 6}, {"fxaa": True}, {"bloom": True},
           {"crt": 96}, {"motion_blur": 70}, {"chromatic": 3}, {"pixelate": 4})
target = sys.argv[1] if len(sys.argv) > 1 else None
with solaros.jet.open(target) as scene:
    scene.background(0x081020)
    material = scene.material(color=0xa0ffff, shading="phong", specular=100, gloss=32)
    cube = scene.primitive(kind="cube", width=180, height=180, depth=180, material=material)
    for frame in range(360):
        if solaros.should_exit():
            break
        if frame % 45 == 0:
            scene.postfx_clear()
            scene.postfx(**presets[(frame // 45) % len(presets)])
        scene.object_set(cube, rx=frame % 360, ry=(frame * 2) % 360)
        scene.render()
        solaros.time.sleep_ms(1)
