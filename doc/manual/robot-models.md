+++
id = "robot-models"
title = "Robot models and transforms"
section = "hardware"
summary = "Load URDF models, query transforms and run declared joint and differential-drive components"
keywords = "robot model URDF Xacro transform quaternion forward kinematics frames solar_os diff_drive odometry streams"
packages_any = ["service_robot"]
+++
# Robot models and transforms

The optional `robot` native library loads expanded URDF into a link/joint tree.
It supplies rigid-transform math, explicit joint-position updates, forward
kinematics, and frame-to-frame point and direction conversion. It works without
connected motors, controllers, ROS, or a display. Install a compatible robot
library from a signed module catalog before loading a description.

Author the description in a desktop ROS 2 package if convenient. Expand Xacro
on the desktop and copy the resulting URDF to the device:

```sh
xacro solbot.urdf.xacro -o solbot.urdf
```

The device reads the flat XML file. It does not run Xacro, resolve ROS package
paths, or load mesh files. Visual, collision, inertial, material, transmission,
Gazebo and `ros2_control` elements are accepted as metadata and omitted from the
in-memory kinematic model. This interface does not render or simulate geometry.

## Device frames

Associate an OS device with a URDF link in the existing extension:

```xml
<link name="imu_link"/>
<joint name="imu_joint" type="fixed">
  <parent link="base_footprint"/>
  <child link="imu_link"/>
  <origin xyz="0 0 0.02" rpy="0 0 0"/>
</joint>
<solar_os version="1">
  <device name="imu0" frame="imu_link"/>
</solar_os>
```

The example pose is a temporary frame 20 mm above `base_footprint` with zero
rotation. Replace it with the physical mounting pose when the IMU is mounted.
Use one `solar_os` block per robot, adding device bindings alongside joint and
drive declarations. Device names must be unique; frames must name existing
links. Up to 16 devices can be associated, and several devices can share a frame.

```text
expansion attach mpu6050 imu0 i2c=i2c0 addr=0x68
robot model load solbot.urdf
robot model devices
robot model frame imu0
robot model frame imu.imu0
robot model transform base_footprint imu_link
imu sample imu0
stream echo imu.imu0 --once
```

Frame associations are queryable after model load and require no `robot start`.
Loading does not attach drivers, claim sensors, or start sampling. Unattached
devices may be declared so geometry is usable without hardware. A device-name
query works while detached; a stream-name query resolves a registered stream's
device. Camera, IMU and future range devices use the same binding syntax.
Samples remain in the sensor's local coordinates, with no frame ID added to the
source. Consumers resolve the binding and apply the existing model transform;
the service does not rotate samples or use measured attitude to move the frame.
Dynamic paths require current joint state, as other transform queries do.

Python and Lua expose `solaros.robot.model_frame(device_or_stream)`, which returns
the frame name. Pass it to `solaros.robot.model_transform(target, frame)` for the
transform. C clients use `solar_os_robot_model_frame()` in the OS frontend or
the native library's appended `model_device` / `model_device_frame` API entries.
Older robot libraries can still load models but do not support frame lookup;
update the library to use these entries.

## Load and inspect

```text
robot model load solbot.urdf
robot model info
robot model links
robot model joints
```

Tab completes model subcommands and the path after `model load`. With a model
loaded, it also completes both frame arguments of `transform`, `point` and
`vector`, and movable joint names in each joint/value pair after `model set`.
Fixed joints and joints already supplied in the batch are omitted from `set`
suggestions. Completion does not load a model or acquire hardware.

Loading parses and validates the document before replacing the resident model.
It checks unique link and joint names, parent/child references, a single root,
connectivity, cycles, duplicate fields, supported joint types, finite numeric
values, axes and limits. A failed read or parse leaves the previous model intact.
XML errors identify the line, column and offending field, for example:

```text
line 6, column 5: Unknown param attribute 'valule' on <param name='kd'>
```

Here the attribute must be `value`. Loading does not create controllers, attach
devices, claim hardware or enable outputs. Existing independent joint
controllers keep running.

There is one device-wide resident model shared by the shell, Python and Lua.
Loading another valid model replaces it and clears the old model's joint states.
The model remains available until explicit unload:

```text
robot model unload
```

Ending a script closes its hardware joints but leaves the shared model available.
Unload the model before unloading the robot library.

## Joint states and forward kinematics

Fixed joints have known positions immediately. Continuous, revolute and
prismatic joints initially have unknown positions. An unset joint is not assumed
to have a measured zero position.

After `robot start`, URDF-bound continuous joints use measured encoder positions
automatically. Transform, point and vector queries and model inspection all use
the latest joint feedback. Positions are radians relative to the encoder count
at enable, plus the configured `position_zero`; this does not reset the hardware
counter or establish an absolute wheel angle. The first valid sample makes the
position available even while the controller waits for its first command.
Queries do not command movement.

Missing, stale or faulted feedback makes the bound position unavailable. Stopping
the joint also clears its model position; restarting establishes a new relative
reference. Revolute and prismatic joints require referenced position feedback;
relative encoder counts alone cannot establish their absolute pose. A servo
command is not measured position feedback.

Supply model state explicitly for unbound joints or while the robot is stopped,
for example:

```text
robot model set left_wheel_joint 0 right_wheel_joint 0
robot model transform base_footprint left_wheel_link
robot model set left_wheel_joint 1.57079632679
robot model transform base_footprint left_wheel_link
```

Rotary positions are radians; prismatic positions are metres. Bounded joints
require valid URDF limits. Supplied state may exceed those limits so the model
can still describe an actual mechanism beyond its permitted travel. Control
components own command-limit enforcement. Continuous joints accept finite
unwrapped angles. A `set` command validates every name and
value before updating any joint. Duplicate names in one batch are rejected.
These updates describe a pose; they never command a motor or servo.
Manual updates to runtime-bound joints are rejected so they cannot replace
measured state. Stop the robot before supplying a manual pose for those joints.

URDF origins use `Rz(yaw) * Ry(pitch) * Rx(roll)`. Joint motion follows the origin
in the joint's local axis. The axis defaults to +X and supplied nonzero axes are
normalized. Transform queries walk the path between the requested links.
Unknown joints on that path return an invalid-state error naming an affected
joint in the shell. Unknown joints above
their common ancestor do not prevent a local lookup. A frame-to-itself lookup
returns identity even when that frame's pose relative to the root is unknown.

This is current-state forward kinematics. There is no transform history,
timestamp interpolation, ROS topic publication, external-frame insertion or
inverse solver in the model interface. The
separate diff_drive component below supplies measured planar odometry.

## Transform direction

The first frame is the target; the second is the source. A lookup returns
`T_target_source`, mapping coordinates expressed in source into target:

```text
robot model transform base_link left_wheel_link
robot model point base_link left_wheel_link 0 0 0
robot model vector base_link left_wheel_link 1 0 0
```

`point` applies rotation and translation. `vector` applies rotation only.
Transform output contains translation in metres and a normalized quaternion in
`x y z w` order. Composition `A * B` applies B first, then A.

For the supplied Solbot model at zero wheel angles, the left wheel centre is
`[0, 0.0350641, 0]` in `base_link`, and the caster centre is
`[-0.0747, 0, 0.006]` in `base_footprint`. These are nominal model coordinates.

## SolarOS extension

One optional `<solar_os version="1">` child of `<robot>` declares rotary motor
velocity controllers and components alongside the standard model:

```xml
<solar_os version="1">
  <joint name="left_wheel_joint" command="velocity">
    <motor device="left" polarity="1"/>
    <encoder device="left_enc" ppr="70" placement="motor" polarity="1"/>
    <transmission gear_ratio="30"/>
    <param name="kp" value="0.04"/>
    <param name="ki" value="0.03"/>
    <param name="kd" value="0"/>
    <param name="max_drive" value="0.2"/>
    <param name="velocity_limit" value="3"/>
    <param name="command_timeout_ms" value="150"/>
  </joint>
</solar_os>
```

Add a matching right-wheel declaration with devices `right` and `right_enc`.
The loader validates the tree, joint references, controller parameters,
calibration, duplicate declarations and exclusive motor/encoder bindings. It
retains the extension's XML verbatim for inspection:

```text
robot model metadata
```

Loading creates no controllers and claims no hardware. Start them explicitly:

```text
robot model load /sdcard/solbot.urdf
robot start
robot joint list
robot stop
robot model unload
```

`robot start` creates, configures and enables the declared controllers and
components. Device availability is checked at startup.
Motor joints create named PID instances in the OS controller service. Inspect
them with `controller list` or `controller status <joint>` (`owner=robot`).
Calibration and bindings apply to the OS encoder, motor and controller services.
Their previous settings are restored before release;
failed cleanup retains the settings/claims for a stop retry.
Outputs stay stopped until fresh commands arrive; the configured command
watchdog applies. A startup failure closes every controller created by that
attempt. If a device stop or lease release fails, ownership is retained: retry
`robot stop` before starting, replacing or unloading the model.

`robot stop` releases only model-owned controllers and leaves the model loaded.
Separately created joints are unaffected. Commands, status and disable remain
available through `robot joint`; model-owned joints must be stopped together
before changing bindings or configuration. They survive interpreter cleanup.

Each binding requires `command="velocity"`, a motor, an encoder, positive PPR
and an explicit gear ratio. Encoder placement is `motor` (default) or `output`;
decoder factor comes from the actual provider. Optional `<param name="..."
value="..."/>` elements use the existing `robot joint configure` keys. Defaults
match manually created joints. Parameters are validated together, in any order;
duplicate keys, unknown keys/attributes and malformed values are rejected.
Do not repeat calibration or polarity as both an attribute and a parameter.
Up to eight bindings are allowed; bound joint names must fit 23 bytes.

Verify example calibration, gains and polarities for the mounted hardware.
Servo controllers remain available through the manual joint API; position and
prismatic bindings are unsupported here.

## Differential drive

Declare one drive after its two velocity-controller bindings:

```xml
<diff_drive name="base" left_joint="left_wheel_joint" right_joint="right_wheel_joint">
  <geometry wheel_radius="0.0145" wheel_separation="0.0701282"/>
  <odometry frame="odom" base_frame="base_footprint"/>
  <param name="command_timeout_ms" value="300"/>
</diff_drive>
```

Place this block inside `<solar_os version="1">`. One drive is supported per
model. Loading validates geometry, controller references and frame names. Both
wheels must have velocity bindings; a wheel cannot belong to another active drive.
`robot start` activates the drive after enabling its joints. It starts waiting
for a body command. `robot stop` stops both outputs, releases the joints, and
removes the component streams and parameters. Close stream readers and writers
before stopping. If cleanup fails, close those handles and retry `robot stop`.

Commands use forward linear X velocity in m/s and left-turn angular Z velocity
in rad/s. Wheel commands are `(linear - angular * separation / 2) / radius`
and `(linear + angular * separation / 2) / radius`, in joint rad/s. If either
wheel would exceed its velocity limit, both commands are scaled together to
preserve curvature. Existing joint PID, acceleration, polarity and drive limits
still apply. Direct commands/disable on a drive-owned wheel are rejected; use
the body-command streams or `robot stop`.

```text
stream status diff_drive.base.cmd_linear
control parameter get diff_drive.base.linear_velocity_limit
stream write diff_drive.base.cmd_linear 0.02 diff_drive.base.cmd_angular 0 --rate 20 --duration 2
stream echo diff_drive.base.odom_x --once
```

This publishes both body-command axes at 20 Hz for two seconds. Without
`--rate`, `stream write` updates each axis only once. Commands expire after
the configured timeout; 300 ms is the default. Ending publishing sends no extra
value, and the drive stops when its command timeout expires. A timeout pauses
both wheel controllers with output zero, retaining ownership and feedback.
Fresh body commands resume operation. A wheel feedback/control fault stops the pair and
latches a drive fault; stop/start the robot to recover.

Two float32 command streams are published:

| Stream | Unit |
| --- | --- |
| `diff_drive.base.cmd_linear` | m/s |
| `diff_drive.base.cmd_angular` | rad/s |

Both are duplex: writing supplies a command, and reading returns the last
accepted command. Readback is not measured motion; use the odometry streams
below for feedback. Each accepts one writer and any number of observers.
`stream status` shows each command's allowed range; an out-of-range
`stream write` reports the rejected
value, range and unit without renewing the command timestamp. Both axes must
have fresh accepted samples before wheel commands are produced. Each axis has
its own acquisition timestamp: publishing linear repeatedly does not renew a
stale angular command. Publish both periodically, as in the example above.
Opening or closing a writer does not itself activate output. Invalid values do
not renew freshness. These two scalar inputs are not an atomic vector
transport: a writer can briefly combine a new axis with the previous fresh axis.

Owned parameters appear under `diff_drive.base`: `wheel_radius`,
`wheel_separation`, `linear_velocity_limit`, `angular_velocity_limit` and
`command_timeout_ms`. Change them in URDF, then stop/load/start. Optional
velocity-limit params use m/s and rad/s. Without them, body limits are derived
from the smaller wheel velocity limit and the geometry. For example, a wheel
radius of 0.0145 m and a wheel velocity limit of 4 rad/s give a default linear
limit of 0.058 m/s. `control parameter get` and `set` complete registered paths;
robot-owned configuration rejects direct changes while the component exists.
Streams and parameters are created on start, not model load. Component names
fit 23 bytes.

Odometry uses measured wheel displacement with the configured encoder scaling
and polarities. Absolute joint referencing is unnecessary. Both wheel samples
must advance before an update; their average acquisition time timestamps the
result. Stale or invalid wheel feedback faults the drive.
Pose integration follows the circular arc between samples, with +X forward
and counterclockwise yaw. Each start resets pose to zero using the current
wheel positions as its baseline.

Read-only odometry streams share that acquisition timestamp:

| Stream suffix under `diff_drive.base` | Unit |
| --- | --- |
| `odom_x`, `odom_y` | m |
| `odom_yaw` | rad |
| `odom_linear` | m/s |
| `odom_angular` | rad/s |

The scripting status API includes frames, pose, measured velocities, validity
and sample time.
The default odometry frame is `odom`; the base defaults to the URDF root. The
base frame should describe the axle-centred, forward-facing body reference.
These outputs provide a planar odom-to-base estimate for a bridge; they do not
insert an external frame into `robot model transform`, publish ROS messages,
estimate covariance or compensate wheel slip.

## Python and Lua

Both runtimes expose these functions on `solaros.robot`:

| Function | Result |
| --- | --- |
| `model_load(path)` | Load a URDF file; return model info. Use an absolute device path. |
| `model_unload()` | Close the shared model and release its lease. |
| `model_info()` | Name, root, link-name list, joint records, known-joint count and extension size. |
| `model_set_joint(name, position)` | Supply one model joint position. |
| `model_transform(target, source)` | `translation` and `rotation` arrays; quaternion order is x,y,z,w. |
| `model_point(target, source, x, y, z)` | Three coordinates after rotation and translation. |
| `model_vector(target, source, x, y, z)` | Three coordinates after rotation only. |
| `start()` | Start the loaded model's controllers and components. |
| `stop()` | Stop model-owned controllers and components; retain the model. |
| `diff_drive_command(name, linear, angular)` | Update both body-command axes atomically, in m/s and rad/s. Rejects competing writes while either command stream has a writer. |
| `diff_drive_status(name)` | Configuration, state, errors, body commands, wheel targets, measured pose and velocity, validity and sample timestamps. |

Joint records include `name`, `parent`, `child`, numeric `kind`, `position` and
`position_known`. Kinds are 0 fixed, 1 revolute, 2 continuous and 3 prismatic.
Python returns lists/dictionaries; Lua returns tables with 1-based arrays.

```python
import solaros

model = solaros.robot.model_load("/solbot.urdf")
solaros.robot.model_set_joint("left_wheel_joint", 0.0)
pose = solaros.robot.model_transform("base_link", "left_wheel_link")
centre = solaros.robot.model_point("base_link", "left_wheel_link", 0, 0, 0)
solaros.robot.model_unload()
```

Lua uses the same names after `local solaros = require("solaros")`.

## URDF limits

Install compatible firmware and a matching robot library to use model and
component commands.

Limits are 128 KiB XML, 64 links, 63 joints, 63-byte names, 32 XML nesting levels,
24 attributes per element, 255-byte decoded attribute values, and 32 KiB retained
SolarOS XML. Oversized input is rejected rather than truncated. The parser accepts
UTF-8, XML comments, processing instructions and predefined/numeric entities.
DTD and external entities are unsupported. Floating, planar and mimic joints
are explicitly unsupported. The model uses float transform math and has no
dependency on ROS libraries. Native API details are in the
[robot library README](https://github.com/nilseuropa/solar_os/blob/main/modules/robot/README.md).

## Quick reference

Load expanded URDF with `robot model load <file.urdf>`. Inspect the model and
supply joint positions with `robot model set` before querying transforms.
`robot start` activates declared controllers and components; `robot stop`
releases them while retaining the model. Publish component commands through
streams, using `stream write <stream> <value> [...] --rate <Hz>` for periodic
updates. Both differential-drive command axes must stay fresh. Read odometry
with `stream echo`. Stop the robot before replacing or unloading its model.
