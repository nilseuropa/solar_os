#!/usr/bin/env python3
"""Read an attached MPU6050 and its URDF frame without starting the robot.

Requires a Telnet shell, FTP root exporting /sdcard, imu0 attached, and a loaded
model associating imu0 with imu_link 20 mm above base_footprint. Does not change
the mounting geometry, attach hardware, start controllers, or command motors.
"""
import argparse
import ftplib
import io
from pathlib import Path
import uuid

from osc_send_smoke import command

SOURCE = '''local s=require("solaros")
local imu,streams,robot=s.imu,s.streams,s.robot
assert(#robot.joints()==0, "stop the robot before this test")
local names=imu.list()
local found=false
for _,v in ipairs(names) do if v.name=="imu0" then
  assert(v.driver=="mpu6050" and v.acceleration and v.angular_velocity and not v.orientation)
  found=true
end end
assert(found)
local a=imu.sample("imu0",250)
assert(a.acceleration_m_s2 and a.angular_velocity_rad_s and not a.orientation)
local function norm(v) return math.sqrt(v.x*v.x+v.y*v.y+v.z*v.z) end
assert(norm(a.acceleration_m_s2)>7 and norm(a.acceleration_m_s2)<13)
assert(robot.model_frame("imu0")=="imu_link")
assert(robot.model_frame("imu.imu0")=="imu_link")
local t=robot.model_transform("base_footprint","imu_link")
assert(math.abs(t.translation[1])<1e-6 and math.abs(t.translation[2])<1e-6 and math.abs(t.translation[3]-.02)<1e-6)
assert(math.abs(t.rotation[4]-1)<1e-6)
print("IMU_SAMPLE_AND_FRAME_PASS",a.timestamp_us,norm(a.acceleration_m_s2))
assert(streams.info("imu.imu0").format=="imu-v1-le")
local handles={}
local ok,err=pcall(function()
  handles[1]=streams.open("imu.imu0")
  handles[2]=streams.open("imu.imu0")
  assert(not pcall(s.expansion.detach,"imu0"), "detach must refuse open readers")
  assert(not pcall(streams.read,handles[1],51,250), "undersized read must fail")
  local previous=0
  for i=1,12 do
    local raw=streams.read(handles[1+i%2],256,250)
    assert(#raw==52)
    local ts,valid,ax,ay,az,gx,gy,gz,qw,qx,qy,qz=string.unpack("<I8I4ffffffffff",raw)
    assert(ts>previous and valid==3)
    local mag=math.sqrt(ax*ax+ay*ay+az*az)
    assert(mag>7 and mag<13)
    assert(gx==gx and gy==gy and gz==gz and qw==0 and qx==0 and qy==0 and qz==0)
    previous=ts
  end
  print("IMU_STREAM_PACKET_AND_LIFETIME_PASS",previous)
end)
for _,h in ipairs(handles) do streams.close(h) end
assert(ok,err)
assert(streams.info("imu.imu0").active_handles==0)
assert(#robot.joints()==0)
print("IMU_SMOKE_PASS")
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--device-root", default="/sdcard")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    name = "imu-hil-" + uuid.uuid4().hex[:8] + ".lua"
    remote = args.device_root.rstrip("/") + "/" + name
    uploaded = False
    try:
        with ftplib.FTP(args.host, timeout=12) as ftp:
            ftp.login()
            ftp.storbinary("STOR " + name, io.BytesIO(SOURCE.encode()))
            uploaded = True
            received = bytearray()
            ftp.retrbinary("RETR " + name, received.extend)
            assert received == SOURCE.encode(), "upload mismatch"
        transcript = command(args.host, "lua " + remote, 15)
        print(transcript)
        (args.output / "console.log").write_text(transcript)
        assert "IMU_SMOKE_PASS" in transcript and "error:" not in transcript, transcript
    finally:
        if uploaded:
            with ftplib.FTP(args.host, timeout=12) as ftp:
                ftp.login()
                ftp.delete(name)


if __name__ == "__main__":
    main()
