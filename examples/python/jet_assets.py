"""Import an OBJ cube, MTL material, and a tiny BMP from memory."""
import struct
import sys
import solaros

obj = """v -1 -1 -1
v 1 -1 -1
v 1 1 -1
v -1 1 -1
v -1 -1 1
v 1 -1 1
v 1 1 1
v -1 1 1
vt 0 0
vt 1 0
vt 1 1
vt 0 1
usemtl checker
f 1/1 4/4 3/3 2/2
f 5/1 6/2 7/3 8/4
f 1/1 5/2 8/3 4/4
f 2/1 3/4 7/3 6/2
f 4/1 8/2 7/3 3/4
f 1/1 2/2 6/3 5/4
"""
mtl = "newmtl checker\nKd 1 1 1\nillum 1\nmap_Kd checker.bmp\n"
# Two bottom-up BGR rows, padded to four-byte boundaries.
pixels = bytes((255, 0, 0, 255, 255, 255, 0, 0, 0, 0, 255, 0, 255, 0, 0, 0))
bmp = struct.pack("<2sIHHIIIIHHIIIIII", b"BM", 70, 0, 0, 54, 40,
                  2, 2, 1, 24, 0, 16, 0, 0, 0, 0) + pixels
target = sys.argv[1] if len(sys.argv) > 1 else None
with solaros.jet.open(target) as scene:
    texture = scene.load_bmp(bmp, name="checker.bmp", address="clamp")
    materials = scene.load_mtl(mtl)
    cube = scene.load_obj(obj, scale=0.7, name="imported cube")
    for frame in range(360):
        if solaros.should_exit():
            break
        scene.object_set(cube, rx=frame % 360, ry=(frame * 2) % 360)
        scene.render()
        solaros.time.sleep_ms(1)
