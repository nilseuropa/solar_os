# AOgmaNeo source

Upstream: https://github.com/ogmacorp/AOgmaNeo

Revision: `645a54ace656b0ac2476a56a0dac19faacbd87ab` (`master`).
The `aogmaneo/` directory contains upstream `source/aogmaneo/`.
Copyright belongs to Ogma Intelligent Systems Corp. The upstream license is
CC BY-NC-SA 4.0; retain `AOGMANEO_LICENSE.md`, `LICENSE.md` and `AUTHORS.md`
with redistributed sources. Commercial use requires an appropriate license
from the copyright holder.

SolarOS changes:

- Use `uint64_t` for PCG state and constants on 32-bit targets, and mask both
  rotation counts.
- Make `Array` resize and copy allocation-safe: build replacement storage before
  exchanging ownership, clean up partial element copies, and reject negative
  lengths.

The module compiles with `USE_STD_MATH` and without `USE_OMP`. The C provider
owns input validation, model handles, random state, call serialization and
exception containment. It does not expose upstream binary deserialization.
