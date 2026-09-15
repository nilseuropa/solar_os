# Bosch BHy2 SensorAPI

These files are vendored from Bosch Sensortec's
[`BHY2_SensorAPI`](https://github.com/boschsensortec/BHY2_SensorAPI) at commit
`177681d120ac42760c9f19505697ccd5c7103e87`:

- `bhy2.c`, `bhy2.h`, `bhy2_defs.h`, `bhy2_hif.c`, and `bhy2_hif.h`
- `BHI260AP.fw`, the standard BHI260AP RAM firmware
- `LICENSE`, Bosch's BSD-3-Clause license

Run `python3 scripts/vendor_bhy2.py` to refresh the checked-in files from the
pinned revision. The script verifies every file's SHA-256 digest before it
writes the vendored copy.
