# Native C project

Copy this directory to a writable device directory, for example
`/dev/cc-project`, and install `cc` and `hello-library`. Then run:

```text
cd /dev/cc-project
edit src/main.c
cc build
load build/demo.elf
```

The two source files share `include/answer.h`. The helper acquires the installed
`hello-library` provider, calls its versioned addition API and releases the
lease. A successful run prints `PROJECT_BUILD_PASS: library=42`.

`cc.toml` declares the source list, include directory, output and exact installed
SDK dependency. Relative paths belong to the project directory, so
`cc build /dev/cc-project/cc.toml` also works from another directory. Builds
create `build/` after successful compilation and preserve the last executable
on build errors or cancellation. See the [compiler guide](../../../../cc/README.md)
for the supported project format and resource limits.
