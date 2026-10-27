"""Primitives, a gradient sky, and a moving point light."""
import sys
import solaros

target = sys.argv[1] if len(sys.argv) > 1 else None
with solaros.jet.open(target) as scene:
    scene.gradient((0x102040, 0xffffff))
    glossy = scene.material(color=0x2192ff, shading="phong", specular=80, gloss=32)
    sphere = scene.primitive(kind="sphere", radius=80, segments=16, material=glossy)
    pyramid = scene.primitive(kind="pyramid", width=130, height=160, color=0xff9020)
    scene.object_set(sphere, x=-100, z=100)
    scene.object_set(pyramid, x=100, z=100)
    lamp = scene.point_light(x=-100, y=100, z=-100, color=0xffd080, range_max=700)
    for frame in range(360):
        if solaros.should_exit():
            break
        scene.object_set(sphere, ry=frame % 360)
        scene.object_set(pyramid, ry=(-frame * 2) % 360)
        scene.point_light_set(lamp, x=(frame % 200) - 100)
        scene.render()
        solaros.time.sleep_ms(1)
