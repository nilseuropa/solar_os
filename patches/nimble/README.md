# Project-local NimBLE overlay

SolarOS enables `CONFIG_BT_NIMBLE_DYNAMIC_SERVICE` so applications can
register GATT services without disconnecting keyboard or generic client links.
The script contract is documented in [the application server API](../../doc/ble-server.md).

The ESP-IDF 5.5.5 dynamic registration path requires additional failure handling.
`scripts/patch_nimble.py` validates SHA-256 hashes of `ble_gatts.c` and
`ble_att_svr.c`, then generates patched copies under the target build directory.
`overlay.cmake` replaces exactly those two sources on this project's `bt` target.
The installed SDK is never edited. Unsupported source revisions fail configuration
with a review-required error; do not bypass the hashes on an SDK update.

## Stale build configuration

Bluetooth-enabled builds require `CONFIG_BT_NIMBLE_ENABLED=y`. Configuration
fails with an explicit error if the active SDK configuration selects another
host. Existing generated `sdkconfig.<environment>` files take precedence over
the checked-in defaults and can retain Bluedroid settings from an older build.
Move the affected generated file to a backup location, then rebuild that
environment to regenerate it. Preserve `sdkconfig.defaults` and
`sdkconfig.defaults.*`; review any intentional local configuration overrides
before reapplying them to the regenerated file.

## Registration guarantees

- Registration holds the host lock on every mutation and balanced return path.
- It does not overwrite or free pending startup service definitions.
- Failure removes all newly appended ATT attributes, service entries and global
  and per-connection subscription records. Existing services, connections and
  subscription flags remain intact.
- Failed registration restores the ATT high-water mark and configurable-characteristic
  count. Output value handles in the rejected definition array are cleared to zero.
- Registration callbacks are buffered until successful commit; no callback or
  Service Changed indication is emitted for a rejected transaction.
- ATT handle exhaustion returns `BLE_HS_ENOMEM` instead of asserting or wrapping.
- Service deletion updates the configurable-characteristic count. Dynamic
  connection admission permits heap fallback; connection initialization still
  returns allocation failure normally if neither pool nor heap can satisfy it.

Call runtime registration only after host startup has completed. Definition
arrays and UUID/value-handle storage must remain valid for the
registered lifetime, as required by NimBLE. They must not alias live service
definitions or their output storage. Registration callbacks must not re-enter
database mutation. Successful service deletion does not recycle exposed ATT
handles: only **failed, unobserved** registration handles are rolled back.
Services with included dependencies must be removed in dependency-safe order.
Statistics are attempt counters, not part of the rollback contract.

## Validation

In the sibling `solar_os_test` checkout:

```sh
python3 -B -m unittest discover -s tests -p 'test_ble_sdk_patch.py'
```

The native harness extracts the patched transaction, original SDK registration
rounds, ATT registration/deregistration/rollback and service deletion functions.
It models characteristic encoding, memory pools and two existing connections;
it does not emulate the controller or prove radio coexistence. ASan/UBSan cover
every allocation point with both transient and persistent failure, then retry,
delete and re-register. Cases cover enabled/disabled registration callbacks,
invalid definitions, startup isolation, handle exhaustion and allocation failure
during connection initialization. Both static and dynamically allocated SDK
context configurations are compiled and tested. Python checks
cover SDK drift rejection, output isolation and idempotent generation.

Target validation remains necessary: keyboard typing, disconnect/reconnect and
sleep/wake, and application server/client coexistence.
