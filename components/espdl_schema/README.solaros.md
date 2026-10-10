# ESP-DL file verification

`espdl.fbs` is from Espressif ESP-DL v3.3.13, commit
`6d3c4da1087d35916858dd0f9b3e9d6a85186e11` (MIT).
`include/espdl_generated.h` was generated with `flatc --cpp espdl.fbs` 2.0.8.
The FlatBuffers headers are 2.0.8 from Ubuntu's `libflatbuffers-dev`
2.0.8+dfsg1-6build1 (Apache-2.0), with trailing whitespace normalized.
License texts are included.

The verifier checks the single-model EDL1/EDL2 envelope, FlatBuffer bounds, and
bounded static tensor interfaces before the native ESP-DL loader receives it.
It does not provide a sandbox or validate every operator's semantic attributes.
Models must be exported for ESP32-S3 and the pinned ESP-DL operator set.

`scripts/patch_espdl.py` applies version-checked, idempotent changes to the
managed ESP-DL component: initialize the default model's context before
explicit load/build; throw on failed custom C++ object allocations; reserve
the execution-plan vector before creating modules; and reclaim temporary
memory-manager / tensor-planning objects during exception unwinding. S3
defaults enable C++ exceptions with an emergency allocation pool. These fixes
do not replace validation of operator-specific exported graph contracts.
