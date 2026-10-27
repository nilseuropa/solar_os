# Native library consumer check

Build the `elf` target to produce `solaros_native_library_check.app.elf`.
With `hello-library` installed, run `load /path/library-check.app.elf`.
Success prints `LIBRARY_CHECK_OK 25 cycles`.

The app checks missing providers, ABI and table-size rejection, shared API
identity, independent retained leases, unload protection, stale tokens, and
25 load/unload cycles. It releases all its leases before returning.
Run with `shared` while `library-user` is running to check concurrent consumers
without unloading the image protected by the job's leases.
