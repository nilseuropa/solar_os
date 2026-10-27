# Resident library client job

Build the `typed_elf` target to produce `solaros_native_library_user.job.elf`.
Install through a signed example catalog. With `hello-library` installed:

```text
job start library-user
job status library-user
pkg unload hello-library
job stop library-user
pkg unload hello-library
pkg remove library-user
```

Unload is refused while the job holds its client and worker leases. The tick
callback uses the library API; stopping the job releases both leases. Installing
the job registers its callbacks but does not acquire the library until start.
