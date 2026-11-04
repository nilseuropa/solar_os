#!/usr/bin/env python3
"""Exercise URDF-owned wheel controllers on a secured, lifted robot.

Requires matching firmware/robot ELF, attached left/right motors and encoders,
FTP on /sdcard and an expanded Solbot URDF. --motion is mandatory. Each motion
window lasts at most two seconds at the URDF's existing drive cap (<= 0.2).
The device script stops model-owned joints in finally; command expiry remains
active throughout. No calibration or gains are changed.
"""
import argparse
import ftplib
import hashlib
import io
import json
from pathlib import Path
import uuid

from robot_joint_smoke import command


def source(model, auxiliary):
    return '''import solaros
import json
r = solaros.robot
c = solaros.controllers
names = ("left_wheel_joint", "right_wheel_joint")
motors = ("left", "right")
aux = __AUX__
assert len(r.joints()) == 0, "Existing joints must be stopped first"
assert aux not in [x["name"] for x in c.list()]
started = solaros.time.uptime_ms()
aux_created = False
def statuses():
    return [r.joint_status(n) for n in names]
def coasted():
    for motor in motors:
        s = solaros.motor.status(motor)
        assert s["mode"] == "coast" and s["drive"] == 0 and not s["blocking_faults"], s
def waiting(expired=False):
    for i, s in enumerate(statuses()):
        assert s["state"] == "waiting" and not s["command_valid"] and s["output"] == 0, s
        assert s["velocity_valid"] and not s["faults"], s
        if expired:
            assert s["last_error"] == -7, s
        k = c.status(names[i])
        assert k["owner"] == "robot" and k["enabled"] and k["waiting"] and not k["faulted"], k
        assert abs(k["output"]) < 0.000001, k
    coasted()
def phase(label, targets):
    before = statuses()
    samples = []
    start = solaros.time.uptime_ms()
    last_sample = start - 100
    while solaros.time.uptime_ms() - start < 2000:
        assert solaros.time.uptime_ms() - started < 35000, "Test deadline exceeded"
        for i in range(2):
            if targets[i]:
                r.joint_command(names[i], targets[i])
        now = solaros.time.uptime_ms()
        if now - last_sample >= 100:
            row = []
            for i, s in enumerate(statuses()):
                assert not s["faults"] and s["velocity_valid"], s
                assert abs(s["output"]) <= 0.20001, s
                if targets[i]:
                    assert s["state"] == "active" and s["command_valid"], s
                    # Allow initial ramp/coast, then reject persistent reversed feedback.
                    if now-start > 700:
                        assert s["velocity"] * targets[i] >= -0.2, s
                else:
                    assert s["output"] == 0, s
                k = c.status(names[i])
                assert k["owner"] == "robot" and k["enabled"] and not k["faulted"], k
                if k["steps"] == s["steps"]:
                    assert abs(k["p"]-s["p"]) < 0.00001 and abs(k["i"]-s["i"]) < 0.00001
                    assert abs(k["output"]-s["output"]) < 0.00001
                row.append([s["raw_count"], s["velocity"], s["output"], s["setpoint"], s["p"], s["i"], s["sample_us"]])
            samples.append([now-start, row])
            last_sample = now
        solaros.time.sleep_ms(40)
    solaros.time.sleep_ms(450)
    after = statuses()
    for i in range(2):
        s = after[i]
        assert s["state"] == "waiting" and not s["command_valid"] and s["output"] == 0, s
        if targets[i]:
            assert s["last_error"] == -7, s
        assert c.status(names[i])["waiting"]
    coasted()
    print("PHASE " + json.dumps(dict(name=label, targets=targets, samples=samples,
        delta=[after[i]["raw_count"]-before[i]["raw_count"] for i in range(2)])))
    for i in range(2):
        if targets[i]:
            assert (after[i]["raw_count"]-before[i]["raw_count"]) * targets[i] > 0, "Encoder did not advance with the command"
try:
    m = r.model_load(__MODEL__)
    assert m["name"] == "solbot" and m["known_joints"] == 2
    c.create(aux, "pid")
    aux_created = True
    r.start()
    solaros.time.sleep_ms(200)
    waiting()
    for s in statuses():
        assert s["ppr"] == 70 and s["gear_ratio"] == 30 and s["decoding"] == 1
        assert s["encoder_counts_per_rev"] == 2100
        assert 0 < s["max_drive"] <= 0.20001 and s["velocity_limit"] >= 2
        assert s["command_timeout_ms"] == 150
    for i, s in enumerate(statuses()):
        rejected = 0
        try:
            solaros.motor.drive(motors[i], 0)
        except OSError:
            rejected += 1
        try:
            solaros.encoder.reset(s["encoder"])
        except OSError:
            rejected += 1
        try:
            c.configure(names[i], "kp", s["kp"])
        except OSError:
            rejected += 1
        assert rejected == 3, "Ownership bypass accepted"
    print("OWNERSHIP_AND_WAITING_PASS")
    for label, target in (("left_positive", (2.0, 0)), ("right_positive", (0, 2.0)),
                          ("left_negative", (-2.0, 0)), ("right_negative", (0, -2.0)),
                          ("both_positive", (2.0, 2.0)), ("opposed", (2.0, -2.0))):
        phase(label, target)
    # Fresh zero commands resume normally without adding a motion window.
    for n in names:
        r.joint_command(n, 0)
    assert all(s["state"] == "active" for s in statuses())
    c.stop(names[0])
    solaros.time.sleep_ms(100)
    s = r.joint_status(names[0])
    assert s["state"] == "fault" and s["output"] == 0 and c.status(names[0])["faulted"], s
    r.stop()
    assert not r.joints() and aux in [x["name"] for x in c.list()]
    coasted()
    r.start()
    solaros.time.sleep_ms(200)
    waiting()
    r.stop()
    assert not r.joints() and aux in [x["name"] for x in c.list()]
    print("OS_STOP_RECOVERY_AND_CLEANUP_PASS")
    print("ROBOT_MODEL_JOINT_PASS")
finally:
    r.stop()
    if aux_created:
        c.delete(aux)
    coasted()
'''.replace('__MODEL__', repr(model)).replace('__AUX__', repr(auxiliary))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--model', default='/sdcard/solbot.urdf')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--motion', action='store_true')
    args = parser.parse_args()
    if not args.motion:
        parser.error('--motion explicitly authorizes the bounded wheel tests')
    if not args.model.startswith('/'):
        parser.error('--model must be an absolute device path')
    args.output.mkdir(parents=True, exist_ok=False)
    token = uuid.uuid4().hex[:8]
    folder = 'robot-control-hil-' + token
    data = source(args.model, 'hil_aux_' + token).encode()
    (args.output / 'smoke.py').write_bytes(data)
    transcript = []

    def run(text, timeout=10):
        output = command(args.host, text, timeout)
        transcript.append(output)
        (args.output / 'console.log').write_text('\n'.join(transcript))
        print(output, flush=True)
        return output

    created = False
    uploaded = False
    try:
        assert 'No joints open' in run('robot joint list'), 'Stop existing joints first'
        with ftplib.FTP(args.host, timeout=10) as ftp:
            ftp.login(); ftp.mkd(folder); created = True
            ftp.storbinary('STOR ' + folder + '/smoke.py', io.BytesIO(data)); uploaded = True
            received = bytearray(); ftp.retrbinary('RETR ' + folder + '/smoke.py', received.extend)
            assert bytes(received) == data
        output = run('python /sdcard/' + folder + '/smoke.py', 45)
        phases = [json.loads(line[6:]) for line in output.splitlines() if line.startswith('PHASE ')]
        (args.output / 'samples.json').write_text(json.dumps(phases, indent=2) + '\n')
        if 'ROBOT_MODEL_JOINT_PASS' not in output or 'Traceback' in output:
            raise RuntimeError('Motion test failed; inspect saved console and motor status')
        assert len(phases) == 6
        print('SCRIPT_SHA256', hashlib.sha256(data).hexdigest())
    finally:
        run('robot stop')
        run('robot joint list'); run('controller list')
        run('motor status left'); run('motor status right')
        if created:
            with ftplib.FTP(args.host, timeout=10) as ftp:
                ftp.login()
                if uploaded:
                    ftp.delete(folder + '/smoke.py')
                ftp.rmd(folder)


if __name__ == '__main__':
    main()
