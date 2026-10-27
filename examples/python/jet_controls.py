"""Cycle full textures, texture-distance fading and scene wireframe."""
import solaros
import sys

target = sys.argv[1] if len(sys.argv) > 1 else None
with solaros.jet.open(target) as scene:
    texture = scene.texture(2, 2, b"\xf8\x00\x07\xe0\x00\x1f\xff\xff")
    material = scene.material(texture=texture, color=0x247de0, shading="unlit")
    cube = scene.cube(150, 150, 150)
    scene.assign(cube, material)
    for frame in range(180):
        if solaros.should_exit():
            break
        phase = frame // 60
        scene.settings(texture_lod=phase == 1, texture_near=350,
                       texture_far=650, wireframe=phase == 2)
        scene.transform(cube, rx=20, ry=frame * 3)
        scene.render()
        solaros.time.sleep_ms(10)
