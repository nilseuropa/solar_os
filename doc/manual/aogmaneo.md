+++
id = "aogmaneo"
title = "AOgmaNeo online learning"
summary = "Use sparse predictive hierarchies, actors and image encoders from C, Python and Lua"
section = "api"
keywords = "aogmaneo ogmaneo machine learning online incremental prediction reinforcement image encoder native library python lua"
packages_any = ["service_native_modules"]
agent_reference_sections = true
+++
# AOgmaNeo online learning

AOgmaNeo is an optional resident native library for ESP32-S3 boards with
executable PSRAM. It supplies sparse predictive hierarchies, reinforcement
learning actors, and image encoding/reconstruction to native C apps and jobs,
Python and Lua.
The provider ID is `aogmaneo`, with provider ABI 1.

## Quick reference

Install from a compatible signed catalog containing this module:

```text
pkg install aogmaneo
cc sdk show aogmaneo/solar_os_aogmaneo_abi.h
```

The library is loaded when a native consumer acquires it. It uses the host's
versioned memory, thread, time and libc services. It creates no background
workers. Execution is synchronous on the calling task; overlapping or reentrant
calls return `SOLAR_OS_NATIVE_ERROR_INVALID_STATE`. Close all model handles and
release all library leases before `pkg unload aogmaneo` or `pkg remove aogmaneo`.

AOgmaNeo is distributed under CC BY-NC-SA 4.0. Commercial use requires an
appropriate license from Ogma Intelligent Systems Corp. See the
[upstream license](https://github.com/ogmacorp/AOgmaNeo/blob/645a54ace656b0ac2476a56a0dac19faacbd87ab/AOGMANEO_LICENSE.md).

## Native API

Include `<aogmaneo/solar_os_aogmaneo_abi.h>` and acquire the provider using
the native `library` service:

```c
uint32_t lease = 0;
const void *table = NULL;
int result = libraries->acquire(SOLAR_OS_AOGMANEO_LIBRARY,
    SOLAR_OS_AOGMANEO_ABI, sizeof(solar_os_aogmaneo_api_v1_t), &lease, &table);
const solar_os_aogmaneo_api_v1_t *api = table;
```

Use the table only after a successful acquire. Keep the lease until all its
models are destroyed. The provider supports up to eight live models in total;
handles are nonzero tokens, and `destroy` accepts either model kind. Shutdown
refuses while any model remains. See [Native libraries](native-library.md)
for service lookup and lease ownership.

| Method | Purpose |
| --- | --- |
| `create` | Create a seeded hierarchy from input and hidden-layer descriptors |
| `step` | Process categorical input, optionally learn, and apply reward/imitation |
| `prediction` | Copy predicted categories or actor actions for one input layer |
| `activations` | Copy prediction/actor activation values, one per cell |
| `values` | Copy actor value estimates, one per column; action inputs only |
| `hidden` | Copy categorical codes from a hidden layer |
| `clear` | Clear a hierarchy's temporal state while retaining learned weights |
| `image_create` | Create a seeded image encoder with one or more byte input layers |
| `image_step` | Encode byte inputs, optionally learn/reconstruct, and copy hidden codes |
| `image_reconstruct` | Reconstruct one visible layer from supplied hidden codes |
| `destroy` | Destroy either model kind and release its allocations |

Hierarchy outputs require a successful step; `clear` invalidates them until
the next step. Prediction and activation methods reject input-only layers.
Learning parameters use the pinned upstream defaults. Provider ABI 1 exposes
native C calls and matching Python/Lua frontends. There is no persistence API.

## Shapes and buffers

Hierarchy inputs and hidden layers have `x` and `y` dimensions in 1..64 and
`z` categorical cells per column in 2..256, with at most 65,536 cells per layer.
There are up to eight inputs and eight hidden layers. Each input is input-only,
prediction, or action. Dendrite counts are even numbers in 2..16. Receptive
radii are in 0..8; a hidden layer's recurrent radius may be `-1` to disable
recurrence. Actor descriptors also require a value size in 2..256, an even value
dendrite count in 2..16 and a history capacity in 2..512.

Categorical input/output buffers contain exactly `x*y` integers in `[0,z)`.
Column order is `y + x*size.y`. Activation buffers contain `x*y*z` floats,
with cell index varying fastest. Actor value buffers contain `x*y` floats.
Reward must be finite and within -1,000,000..1,000,000; imitation strength must
be finite and in 0..1.

Image visible layers additionally accept `z=1`. Each visible input contains
exactly `x*y*z` bytes in `channel + z*(y + x*size.y)` order. Convert camera
frames into this order before encoding. Image hidden outputs contain `x*y`
categorical codes. Reconstruction copies exactly the visible layer's `x*y*z`
bytes.

Every upstream weight array is limited to 4,194,304 elements. Large descriptors
can exceed available PSRAM despite satisfying shape limits; creation returns
`SOLAR_OS_NATIVE_ERROR_NO_MEMORY` when allocation fails. Models use independent
seeded random state. All descriptors and inputs are copied into provider-owned
storage, and outputs are copied to caller buffers. Buffers are borrowed only
for the duration of the call.

Invalid arguments leave models unchanged. If allocation or engine execution
fails during a step, that model becomes unusable. Destroy it and create a new
model. No C++ exception crosses the native API.

## Python and Lua

`solaros.aogmaneo` is available when `service.aogmaneo` is compiled into the
firmware. The Python and Lua groups select this frontend on ESP32-S3 boards
with PSRAM. The engine must also be installed as the `aogmaneo` library.
Creating a model loads the installed ELF without downloading it. A missing
library raises an error directing the caller to `pkg install aogmaneo`.

The two runtimes expose the same functions:

| Call | Result |
| --- | --- |
| `create(inputs, layers, seed=12345)` | Hierarchy handle |
| `step(model, inputs, learn=True, reward=0, mimic=0)` | Process/learn one step |
| `prediction(model, input=0)` | List of predicted categorical codes |
| `activations(model, input=0)` | List of cell activations |
| `values(model, input=0)` | List of actor values; action inputs only |
| `hidden(model, layer=0)` | List of hidden categorical codes |
| `clear(model)` | Reset hierarchy temporal state, retaining weights |
| `image_create(hidden, visible, seed=12345)` | Image-encoder handle |
| `image_step(model, inputs, learn=True, learn_reconstruction=False)` | List of hidden codes |
| `image_reconstruct(model, codes, visible=0)` | Python bytes or Lua binary string |
| `destroy(model)` | Destroy either model kind and release its library lease |

Defaults above use Python spelling. Lua uses `true` and `false` and positional
arguments. Python accepts keyword arguments for `create`, `step`,
`image_create` and `image_step`; pass optional indexes positionally for query
and reconstruction calls. Input, hidden-layer and visible-layer indexes are
zero-based in both languages. Lua list tables use normal one-based array positions.

`inputs` and `layers` in `create` are nonempty lists of up to eight descriptors.
Each descriptor is a Python dictionary or Lua table. `size` is required and is
a three-element list/tuple or Lua table `{x,y,z}`. The other fields default to:

| Descriptor | Fields and defaults |
| --- | --- |
| Hierarchy input | `type=PREDICTION`, `dendrites=4`, `up_radius=2`, `down_radius=2`, `value_size=128`, `value_dendrites=2`, `history_capacity=512` |
| Hidden layer | `dendrites=4`, `up_radius=2`, `recurrent_radius=0`, `down_radius=2` |
| Image visible layer | `radius=2` |

Input types are `solaros.aogmaneo.INPUT`, `PREDICTION` and `ACTION`. Actor
parameters are `value_size`, `value_dendrites` and `history_capacity`.
Seeds are unsigned 32-bit values. Shapes, radii and weight-array limits follow
the native API. Unknown descriptor fields and invalid dimensions raise errors.

`step` receives one list of categorical codes per hierarchy input. `image_step`
receives one byte buffer per visible layer: Python bytes-like objects or Lua
binary strings. Counts and layouts must match the declared shapes. Returned
lists and byte strings are owned by the interpreter.

Handles belong to the current interpreter invocation. A handle from another
run has no meaning. Destroyed handles are rejected and are not reused within
the same invocation. The provider supports eight live models across all
consumers, including Python, Lua and native applications. Each live model pins
the library. Explicitly destroy models when finished; interpreter shutdown
also destroys remaining models on normal exit, script errors and cancellation.
Failed close retains ownership for cleanup retry.

Native operations run synchronously. Cancellation is checked before and after
each operation; it does not interrupt the middle of a learning step.

```python
import solaros

a = solaros.aogmaneo
m = a.create([{"size": (1, 1, 4)}], [{"size": (2, 2, 16)}])
try:
    for tick in range(200):
        a.step(m, [[tick % 4]])
    print(a.prediction(m))
finally:
    a.destroy(m)
```

```lua
local a = solaros.aogmaneo
local m = a.create({{size={1,1,4}}}, {{size={2,2,16}}})
local ok, err = pcall(function()
    for tick=0,199 do a.step(m, {{tick % 4}}) end
    print(a.prediction(m)[1])
end)
a.destroy(m)
if not ok then error(err) end
```

Complete [Python](https://github.com/nilseuropa/solar_os/blob/main/examples/python/aogmaneo.py)
and [Lua](https://github.com/nilseuropa/solar_os/blob/main/examples/lua/aogmaneo.lua)
examples also encode and reconstruct byte images.

## C project

Declare the installed SDK dependency in `cc.toml`:

```toml
sources = ["predict.c"]
output = "predict.elf"
dependencies = [{ id = "aogmaneo", version = "1.0.0" }]
```

The [native C example](https://github.com/nilseuropa/solar_os/tree/main/modules/aogmaneo/examples)
creates a predictive hierarchy, trains it on a repeating sequence, reads a
prediction, destroys the model and releases its lease. Run `cc build` and
`load predict.elf` from the project directory. See [C development](c.md)
for compiler and project options.
