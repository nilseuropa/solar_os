#!/usr/bin/env python3
"""Test an installed robot model API using Solbot, without activating hardware.

Requires compatible firmware/library, Telnet and an FTP server exporting the
device root (normally /sdcard). Uses an owned temporary directory and leaves
the model unloaded. Refuses to replace an already resident model.
"""
import argparse
import ftplib
import hashlib
import io
from pathlib import Path
import re
import socket
import time
import uuid

from robot_joint_smoke import command


ROOT = Path(__file__).resolve().parents[2]
EXTENSION = '''<solar_os version="1">
  <joint name="left_wheel_joint" command="velocity">
    <motor device="left_motor" polarity="1"/>
    <encoder device="left_encoder" ppr="70" placement="motor" polarity="1"/>
    <transmission gear_ratio="30"/>
  </joint>
  <diff_drive name="base" left_joint="left_wheel_joint" right_joint="right_wheel_joint">
    <geometry wheel_radius="0.0145" wheel_separation="0.0701282"/>
    <odometry frame="odom" base_frame="base_footprint"/>
  </diff_drive>
</solar_os>'''


def source(directory):
    return f'''import solaros
import math
r = solaros.robot
directory = {directory!r}
print("ROBOT_MODEL_BEGIN")
before = r.joints()
def near(actual, expected):
    assert abs(actual - expected) < 0.00002, (actual, expected)
try:
    info = r.model_load(directory + "/solbot.urdf")
    assert info["name"] == "solbot" and info["root"] == "base_footprint"
    assert len(info["links"]) == 5 and len(info["joints"]) == 4
    assert info["known_joints"] == 2
    rejected = False
    try:
        r.model_transform("base_link", "left_wheel_link")
    except OSError:
        rejected = True
    assert rejected, "unknown joint was treated as measured zero"
    caster = r.model_transform("base_footprint", "caster_ball_link")
    near(caster["translation"][0], -0.0747)
    near(caster["translation"][2], 0.006)
    print("STATIC_AND_UNKNOWN_PASS")
    r.model_set_joint("left_wheel_joint", 0)
    r.model_set_joint("right_wheel_joint", 0)
    spacing = r.model_transform("right_wheel_link", "left_wheel_link")
    near(spacing["translation"][1], 0.0701282)
    r.model_set_joint("left_wheel_joint", math.pi / 2)
    p = r.model_point("base_link", "left_wheel_link", 1, 0, 0)
    v = r.model_vector("base_link", "left_wheel_link", 1, 0, 0)
    near(p[0], 0); near(p[1], 0.0350641); near(p[2], -1)
    near(v[0], 0); near(v[1], 0); near(v[2], -1)
    inverse = r.model_point("left_wheel_link", "base_link", p[0], p[1], p[2])
    near(inverse[0], 1); near(inverse[1], 0); near(inverse[2], 0)
    print("KINEMATICS_AND_POINTS_PASS", spacing, p, inverse)
    rejected = False
    try:
        r.model_load(directory + "/invalid.urdf")
    except OSError:
        rejected = True
    assert rejected and r.model_info()["known_joints"] == 4
    near(r.model_point("base_link", "left_wheel_link", 1, 0, 0)[2], -1)
    print("FAILED_RELOAD_PRESERVES_MODEL_PASS")
    info = r.model_load(directory + "/extended.urdf")
    assert info["extension_bytes"] > 0 and len(info["joints"]) == 4
    assert len(r.joints()) == len(before), "metadata created hardware controllers"
    print("EXTENSION_METADATA_PASS", info["extension_bytes"])
    for cycle in range(10):
        info = r.model_load(directory + "/solbot.urdf")
        r.model_set_joint("left_wheel_joint", 0)
        near(r.model_transform("base_link", "left_wheel_link")["translation"][1], 0.0350641)
        r.model_unload()
    assert len(r.joints()) == len(before)
    print("MODEL_LIFECYCLE_PASS", 10)
    print("ROBOT_MODEL_PASS")
finally:
    r.model_unload()
'''


def lua_source(directory):
    return f'''local r = require("solaros").robot
local info = r.model_load({(directory + "/solbot.urdf")!r})
assert(info.name == "solbot" and #info.links == 5 and #info.joints == 4)
assert(info.known_joints == 2)
assert(not pcall(r.model_transform, "base_link", "left_wheel_link"))
r.model_set_joint("left_wheel_joint", math.pi / 2)
local p = r.model_point("base_link", "left_wheel_link", 1, 0, 0)
local v = r.model_vector("base_link", "left_wheel_link", 1, 0, 0)
assert(math.abs(p[1]) < 0.00002 and math.abs(p[2] - 0.0350641) < 0.00002 and math.abs(p[3] + 1) < 0.00002)
assert(math.abs(v[1]) < 0.00002 and math.abs(v[2]) < 0.00002 and math.abs(v[3] + 1) < 0.00002)
r.model_unload()
assert(not pcall(r.model_info))
print("ROBOT_MODEL_LUA_PASS")
'''


def complete(host, keys):
    """Exercise real Tab key handling without executing the edited line."""
    data = bytearray()
    with socket.create_connection((host, 23), timeout=5) as connection:
        connection.settimeout(.1)
        deadline = time.monotonic() + .5
        while time.monotonic() < deadline:
            try:
                block = connection.recv(8192)
                if not block:
                    raise RuntimeError("Completion console closed")
                data.extend(block)
            except socket.timeout:
                pass
        if b"Telnet is busy" in data:
            raise RuntimeError("Completion console is busy")
        data.clear()
        connection.sendall(keys.encode())
        deadline = time.monotonic() + .8
        while time.monotonic() < deadline:
            try:
                block = connection.recv(8192)
                if not block:
                    break
                data.extend(block)
            except socket.timeout:
                pass
    time.sleep(.3)
    return re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", data.decode(errors="replace"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--device-root", default="/sdcard")
    parser.add_argument("--urdf", type=Path, default=ROOT / "tests/fixtures/solbot.urdf")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    before = command(args.host, "robot model info")
    (args.output / "before.log").write_text(before)
    if "usage:" in before or "robot model:" not in before:
        raise RuntimeError("Model API missing, or a model is already resident")
    folder = "robot-model-hil-" + uuid.uuid4().hex[:12]
    device = args.device_root.rstrip("/") + "/" + folder
    urdf = args.urdf.read_bytes()
    assert urdf.count(b"</robot>") == 1
    extended = urdf.replace(b"</robot>", EXTENSION.encode() + b"\n</robot>")
    files = {"solbot.urdf": urdf, "extended.urdf": extended,
             "invalid.urdf": b"<robot name='invalid'><link name='a'/><link name='b'/></robot>",
             "smoke.py": source(device).encode(), "smoke.lua": lua_source(device).encode()}
    owned = []
    created = False
    try:
        with ftplib.FTP(args.host, timeout=10) as ftp:
            ftp.login(); ftp.mkd(folder); created = True
            for name, data in files.items():
                path = folder + "/" + name
                owned.append(path)
                ftp.storbinary("STOR " + path, io.BytesIO(data))
                received = bytearray(); ftp.retrbinary("RETR " + path, received.extend)
                if bytes(received) != data:
                    raise RuntimeError("Upload verification failed: " + name)
        transcript = []

        def run(text, timeout=15):
            output = command(args.host, text, timeout)
            transcript.append(output)
            (args.output / "console.log").write_text("\n".join(transcript))
            print(output, flush=True)
            return output

        run("mem")
        output = run("python " + device + "/smoke.py", 30)
        if "ROBOT_MODEL_PASS" not in output or "Traceback" in output:
            raise RuntimeError("Robot model Python smoke failed")
        if "ROBOT_MODEL_LUA_PASS" not in run("lua " + device + "/smoke.lua", 15):
            raise RuntimeError("Robot model Lua smoke failed")
        output = run("robot model load " + device + "/extended.urdf")
        if "Robot model loaded" not in output:
            raise RuntimeError("Shell model loading failed")
        if '<diff_drive name="base"' not in run("robot model metadata"):
            raise RuntimeError("Extension metadata was not retained")
        run("robot model set left_wheel_joint 0 right_wheel_joint 0")
        if "base_link <- left_wheel_link" not in run("robot model transform base_link left_wheel_link"):
            raise RuntimeError("Shell transform failed")
        if "0.035064" not in run("robot model point base_link left_wheel_link 0 0 0"):
            raise RuntimeError("Shell point transform failed")
        completion_checks = (
            ("robot mo\t", "robot model "),
            ("robot model tr\t", "robot model transform "),
            ("robot model load " + device + "/solb\t", "solbot.urdf"),
            ("robot model transform base_link le\t", "left_wheel_link"),
            ("robot model point base_link ri\t", "right_wheel_link"),
            ("robot model set le\t", "left_wheel_joint"),
            ("robot model set left_wheel_joint 0 ri\t", "right_wheel_joint"),
        )
        for index, (keys, expected) in enumerate(completion_checks):
            output = complete(args.host, keys)
            (args.output / ("completion-%02d.log" % index)).write_text(output)
            if expected not in output:
                raise RuntimeError("Completion failed: " + repr(keys) + ": " + repr(output))
        output = complete(args.host, "robot model set \t\t")
        (args.output / "completion-joint-list.log").write_text(output)
        if not all(name in output for name in ("left_wheel_joint", "right_wheel_joint")) or "base_joint" in output or "caster_joint" in output:
            raise RuntimeError("Movable joint completion list is incorrect")
        print("ROBOT_MODEL_COMPLETION_PASS", flush=True)
        run("robot model unload")
        run("pkg unload robot")
        run("mem")
        (args.output / "fixture.sha256").write_text(hashlib.sha256(urdf).hexdigest() + "\n")
        print("ROBOT_MODEL_HIL_PASS", flush=True)
    finally:
        print(command(args.host, "robot model unload"), flush=True)
        if created:
            with ftplib.FTP(args.host, timeout=10) as ftp:
                ftp.login()
                for path in owned:
                    ftp.delete(path)
                ftp.rmd(folder)


if __name__ == "__main__":
    main()
