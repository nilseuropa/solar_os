+++
id = "robot-models"
title = "Robot models and transforms"
section = "hardware"
summary = "Load URDF models and query link transforms with the optional robot library"
keywords = "robot model URDF Xacro transform quaternion forward kinematics frames solar_os"
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
Loading does not create controllers, attach devices, claim hardware, start
workers, or enable outputs. Existing independent joint controllers keep running.

There is one device-wide resident model shared by the shell, Python and Lua.
Loading another valid model replaces it and clears the old model's joint states.
The model holds a native-library lease until explicit unload:

```text
robot model unload
```

Script teardown closes script-owned hardware joints but leaves the shared model
available. The native library cannot unload while the model holds its lease.

## Joint states and forward kinematics

Fixed joints have known positions immediately. Continuous, revolute and
prismatic joints initially have unknown positions. An unset joint is not assumed
to have a measured zero position. Supply positions explicitly, for example:

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

URDF origins use `Rz(yaw) * Ry(pitch) * Rx(roll)`. Joint motion follows the origin
in the joint's local axis. The axis defaults to +X and supplied nonzero axes are
normalized. Transform queries walk the path between the requested links.
Unknown joints on that path return an invalid-state error. Unknown joints above
their common ancestor do not prevent a local lookup. A frame-to-itself lookup
returns identity even when that frame's pose relative to the root is unknown.

This is current-state forward kinematics. There is no transform history,
timestamp interpolation, ROS topic publication, external-frame insertion,
automatic encoder binding, inverse solver, differential-drive execution or
odometry in the model interface.

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
`x y z w` order. Composition `A * B` applies B first, then A. Native transform
functions support aliased input/output, normalize nonzero caller quaternions,
and reject nonfinite or degenerate transforms without changing the output.

For the supplied Solbot model at zero wheel angles, the left wheel centre is
`[0, 0.0350641, 0]` in `base_link`, and the caster centre is
`[-0.0747, 0, 0.006]` in `base_footprint`. These are nominal model coordinates.

## SolarOS extension

One optional `<solar_os version="1">` child of `<robot>` can carry hardware and
drive declarations alongside the standard model:

```xml
<solar_os version="1">
  <joint name="left_wheel_joint" command="velocity">
    <motor device="left_motor" polarity="1"/>
    <encoder device="left_encoder" ppr="70" placement="motor" polarity="1"/>
    <transmission gear_ratio="30"/>
  </joint>
  <diff_drive name="base"
              left_joint="left_wheel_joint"
              right_joint="right_wheel_joint">
    <geometry wheel_radius="0.0145" wheel_separation="0.0701282"/>
    <odometry frame="odom" base_frame="base_footprint"/>
  </diff_drive>
</solar_os>
```

The loader validates XML, the extension version, joint references, duplicate
joint bindings, rotary drive references, and odometry base-frame references.
It retains the extension's XML verbatim for inspection:

```text
robot model metadata
```

The declarations are descriptive metadata in this interface. Device existence,
calibration, control gains and drive geometry are not resolved or activated.
Use the existing `robot joint` operations separately for hardware control.

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

## Native API and limits

The append-only model and transform functions follow the joint-controller prefix
in robot ABI 3 (`solar_os_robot_abi.h`). Model clients must acquire the full new
table size; joint-only clients may request `SOLAR_OS_ROBOT_JOINT_API_SIZE`.
An older robot ELF cannot satisfy model clients. Update the installed library
alongside firmware when using the model commands.

Native callers may keep up to four model handles, independently of the one
shared firmware model. Each handle must be closed while its library lease is
still held. Model state and parser scratch use the native PSRAM allocator;
input XML is consumed synchronously and need not stay alive after load.

Limits are 128 KiB XML, 64 links, 63 joints, 63-byte names, 32 XML nesting levels,
24 attributes per element, 255-byte decoded attribute values, and 32 KiB retained
SolarOS XML. Oversized input is rejected rather than truncated. The parser accepts
UTF-8, XML comments, processing instructions and predefined/numeric entities.
DTD and external entities are unsupported. Floating, planar and mimic joints
are explicitly unsupported. The model uses float transform math and has no
dependency on ROS libraries.

## Quick reference

Use `robot model load <file.urdf>` to load expanded URDF without hardware
activation. Inspect `info`, `links`, `joints` and `metadata`. Supply movable
joint positions with `robot model set <joint> <position>`; unknown positions
block transforms only along the affected path. `robot model transform <target>
<source>` maps source coordinates into target. `point` includes translation;
`vector` rotates a direction only. Python/Lua expose matching `solaros.robot.model_*`
functions. The version-1 SolarOS extension is retained metadata, without device
binding or drive execution. `robot model unload` releases the shared model.
