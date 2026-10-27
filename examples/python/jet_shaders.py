"""Native procedural shaders; optional protected Python pixel callback."""
import solaros
import sys

target = sys.argv[1] if len(sys.argv) > 1 else None
with solaros.jet.open(target) as scene:
    shader = scene.shader(kind="checker", color=0x247de0, secondary=0xffa030, scale=6)
    material = scene.material(shader=shader, shading="unlit")
    cube = scene.cube(140, 140, 140)
    scene.assign(cube, material)
    for frame in range(120):
        if solaros.should_exit():
            break
        scene.transform(cube, ry=frame * 3, rx=20)
        scene.render()
        solaros.time.sleep_ms(10)

    # Python runs once per visible fragment. Keep callback surfaces small.
    def stripes(u, v, params):
        return 0x247de0 if int(u * 8) % 2 else 0xffa030

    scene.shader_set(shader, stripes)
    scene.object_set(cube, scale_x=0.25, scale_y=0.25, scale_z=0.25)
    for frame in range(12):
        if solaros.should_exit():
            break
        scene.transform(cube, ry=frame * 15, rx=20)
        scene.render()
