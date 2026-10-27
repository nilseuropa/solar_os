"""Rotate two shaded cubes. Install the jet library first.

Usage: python /examples/jet.py [display]
Ctrl+] stops the script; scene cleanup also runs on errors.
"""
import sys
import solaros

target = sys.argv[1] if len(sys.argv) > 1 else None
with solaros.jet.open(target) as scene:
    scene.background(0xffffff)
    scene.camera(z=-500, fov=60)
    scene.light(ambient=0x5a5a5a, color=0xfff5e6, intensity=220,
                azimuth=35, elevation=40)
    blue = scene.cube(150, 150, 150, color=0x2192ff)
    orange = scene.cube(80, 80, 80, color=0xff7d00)
    for frame in range(720):
        if solaros.should_exit():
            break
        scene.transform(blue, x=-55, z=100, rx=frame % 360,
                        ry=(frame * 2) % 360, rz=15)
        scene.transform(orange, x=100, y=35, z=30, rx=25,
                        ry=(-frame) % 360, rz=frame % 360)
        scene.render()
        solaros.time.sleep_ms(1)
