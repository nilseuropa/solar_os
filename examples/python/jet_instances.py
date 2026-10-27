"""Six independently rotating cubes share one immutable mesh."""
import sys
import solaros

target = sys.argv[1] if len(sys.argv) > 1 else None
with solaros.jet.open(target) as scene:
    source = scene.cube(70, 70, 70)
    prototype = scene.prototype(source, name="cube")
    scene.remove(source)
    colors = (scene.material(color=0x2192ff), scene.material(color=0xff9020))
    batch = scene.instance(prototype, x=-120, y=-65, material=colors[0])
    for index in range(1, 6):
        scene.instance_add(batch, prototype, x=(index % 3 - 1) * 120,
                           y=-65 if index < 3 else 65, material=colors[index % 2])
    for frame in range(360):
        if solaros.should_exit():
            break
        for index in range(6):
            scene.instance_set(batch, index, rx=(frame + index * 20) % 360,
                               ry=(frame * 2 + index * 30) % 360)
        scene.render()
        solaros.time.sleep_ms(1)
