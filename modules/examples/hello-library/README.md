# Native library example

`hello-library` registers a shared API for unsigned addition modulo 2^32.
It uses library lifecycle ABI 1 and provider ABI 1. Its registration owns no
resources; shutdown is synchronous.

Build with ESP-IDF 5.5 and `cmake --build build --target typed_elf`. The artifact
is `solaros_native_hello_library.library.elf`. Install through a signed example
catalog, or copy it to `/sdcard/modules/libraries/hello-library.elf`.
Copying an ELF is a maintainer operation and bypasses catalog verification.

The `library-check` example app acquires this library, checks compatibility,
shares and retains leases, rejects stale tokens and unloads it over 25 cycles.
`library-user` is a job that holds separate client and worker leases until stopped.
See [Native libraries](../../../doc/manual/native-library.md) for the contract.
