"""A screen-space cursor picks a rotating cube beneath the HUD."""
import sys
import solaros

target = sys.argv[1] if len(sys.argv) > 1 else None
with solaros.jet.open(target) as scene:
    status = scene.status()
    x, y = status["width"] // 2, status["height"] // 2
    cube = scene.cube(180, 180, 180, color=0x2192ff)
    cursor = scene.sprite(x=x - 2, y=y - 2, width=5, height=5, color=0xff0000)
    scene.pick(((x, y),))
    for frame in range(360):
        if solaros.should_exit():
            break
        scene.object_set(cube, rx=frame % 360, ry=(frame * 2) % 360)
        scene.render()
        hit = scene.picks()[0]["hit"]
        scene.sprite_set(cursor, color=0x20ff20 if hit else 0xff0000)
        solaros.time.sleep_ms(1)
