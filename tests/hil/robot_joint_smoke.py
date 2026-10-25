#!/usr/bin/env python3
"""Deploy a robot-joint smoke script over FTP and run it over Telnet.

The firmware and robot ELF must already be installed. Default: claims and
zero-output checks only. --motion permits one bounded test per motor, with
drive <= 0.2 and a two-second command window followed by command-expiry checks.
Run motion tests only with wheels lifted or the mechanism otherwise secured.
"""
import argparse
import ftplib
import io
import math
import re
import socket
import time


def command(host, text, timeout=5):
    connection = None
    initial = b""
    for _ in range(12):
        connection = socket.create_connection((host, 23), timeout=3)
        connection.settimeout(.3)
        try:
            initial = connection.recv(8192)
        except socket.timeout:
            initial = b""
        if b"Telnet is busy" not in initial:
            break
        connection.close()
        connection = None
        time.sleep(.3)
    if connection is None:
        raise RuntimeError("SolarOS remote shell is busy")
    with connection:
        connection.settimeout(.2)
        time.sleep(.2)
        connection.sendall((text + "\r\n").encode())
        end = time.monotonic() + timeout
        data = bytearray(initial)
        while time.monotonic() < end:
            try:
                block = connection.recv(8192)
                if not block:
                    break
                data.extend(block)
                clean = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", data.decode(errors="replace"))
                if text in clean and re.search(r"(?:^|\n)[^ \r\n]+@[^ \r\n]+:[^\r\n]* $", clean):
                    break
            except socket.timeout:
                pass
        result = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", data.decode(errors="replace"))
    time.sleep(.3)  # Allow the single remote shell to finish session cleanup.
    return result


def source(ppr, ratio, motion, friction_bias=0, command_ms=2000):
    return f'''import solaros
print("ROBOT_JOINT_BEGIN")
for motor, encoder in (("left", "left_enc"), ("right", "right_enc")):
    name = "joint-test-" + motor
    created = solaros.robot.joint_create(name)
    try:
        solaros.robot.joint_bind_motor(name, motor, encoder)
        for key, value in (("ppr", {ppr!r}), ("gear_ratio", {ratio!r}),
                           ("motor_polarity", 1), ("encoder_polarity", 1),
                           ("max_drive", 0.2), ("velocity_limit", 3),
                           ("friction_bias_forward", {friction_bias!r}), ("friction_bias_reverse", {friction_bias!r}),
                           ("velocity_deadband", 0.05),
                           ("acceleration_limit", 10), ("kp", 0.04), ("ki", 0.03),
                           ("command_timeout_ms", 150)):
            solaros.robot.joint_configure(name, key, value)
        solaros.robot.joint_enable(name)
        initial_count = solaros.encoder.read(encoder)
        rejected = 0
        try:
            solaros.motor.drive(motor, 0)
        except OSError:
            rejected += 1
        try:
            solaros.encoder.reset(encoder)
        except OSError:
            rejected += 1
        assert rejected == 2, "competing writes were accepted"
        print("CLAIMS", motor, rejected)
        solaros.robot.joint_command(name, 0.01)
        assert solaros.robot.joint_status(name)["target"] == 0, "command deadband did not apply"
        if {motion!r}:
            start = solaros.time.uptime_ms()
            last_print = start
            while solaros.time.uptime_ms() - start < {command_ms!r}:
                solaros.robot.joint_command(name, 2.0)
                if solaros.time.uptime_ms() - last_print >= 80:
                    s = solaros.robot.joint_status(name)
                    print("SAMPLE", motor, s["state"], s["raw_count"], s["velocity"], s["output"], s["feedforward"], s["steps"])
                    assert s["state"] == "active", "joint faulted during command refresh"
                    assert abs(s["output"]) <= 0.20001, "drive exceeded approved cap"
                    last_print = solaros.time.uptime_ms()
                solaros.time.sleep_ms(40)
            solaros.time.sleep_ms(500)
            s = solaros.robot.joint_status(name)
            assert s["state"] == "waiting" and s["last_error"] == -7, "command expiry did not stop output"
            assert s["raw_count"] > initial_count, "encoder did not advance in joint-positive direction"
            print("EXPIRED", motor, s["state"], s["last_error"], s["steps"])
            solaros.robot.joint_command(name, 0)
            assert solaros.robot.joint_status(name)["state"] == "active", "fresh command did not resume control"
        solaros.robot.joint_disable(name)
    finally:
        solaros.robot.joint_close(name)
    s = solaros.motor.status(motor)
    assert s["mode"] == "coast" and s["drive"] == 0, "motor did not return to coast"
    print("CLOSED", motor, s["mode"], s["drive"])
assert not [a for a in solaros.robot.joints() if a["name"].startswith("joint-test-")]
print("ROBOT_JOINT_PASS")
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--ppr", type=float, required=True)
    parser.add_argument("--gear-ratio", type=float, required=True)
    parser.add_argument("--motion", action="store_true")
    parser.add_argument("--friction-bias", type=float, default=0,
                        help="Continuous normalized feedforward bias for both directions (default 0); calibrate separately from breakaway drive")
    parser.add_argument("--command-ms", type=int, default=2000)
    args = parser.parse_args()
    if not all(math.isfinite(x) and x > 0 for x in (args.ppr, args.gear_ratio)):
        parser.error("PPR and gearbox ratio must be finite and positive")
    if not math.isfinite(args.friction_bias) or not 0 <= args.friction_bias <= .2:
        parser.error("Friction bias must be between zero and the 0.2 output cap")
    if not 80 <= args.command_ms <= 2000:
        parser.error("Command window must be between 80 and 2000 ms")
    data = source(args.ppr, args.gear_ratio, args.motion, args.friction_bias, args.command_ms).encode()
    ftp = ftplib.FTP(args.host, timeout=5)
    ftp.login()
    try:
        ftp.mkd("dev")
    except ftplib.error_perm:
        pass
    ftp.storbinary("STOR dev/robot-joint-smoke.py", io.BytesIO(data))
    ftp.quit()
    output = command(args.host, "python /sdcard/dev/robot-joint-smoke.py", timeout=20)
    print(output)
    if "ROBOT_JOINT_PASS" not in output:
        raise SystemExit("joint smoke test failed or timed out; inspect robot joint list and motor status")


if __name__ == "__main__":
    main()
