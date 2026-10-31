# SolarOS review implementation and validation

Branch: `dev/4.17.4`. Base: `30b7c22f`. Firmware source: `db580cd0`.
The version remains `4.17.3`; this work does not publish a release or push commits.
Later commits add tests and this validation record without changing firmware.

## Implemented changes

| Area | Result |
| --- | --- |
| Registry lifetime | Shared initialization for static and optional dynamic mutexes; parameter callback leases and retirement; serialized e-paper adapter attachment/detachment. |
| HTTP | Origin-aware redirect credentials, bounded relative URL resolution, method/body rules, HTTPS downgrade rejection, and maximum-length SC2 discovery URLs. |
| Display bindings | Six e-paper adapters share SPI/GPIO parsing, pin uniqueness checks and rotation validation; controller-specific I/O and power sequences remain local. |
| Downloads | Docs, Playground, native modules and Zoo use one bounded staging downloader with exact size/hash checks and stored-byte readback verification. |
| Publication | Shared single/group file replacement preflights paths, recovers sole backups, rolls back earlier publications on failure and retains failed rollback backups. Docs and Playground adopt it. |
| Shell and bindings | Shared subcommand/option vocabulary feeds help and completion. HTTP and storage conversions are organized by domain for Python and Lua, using the existing common API registration descriptor. |
| Audio | Equal-rate PCM copy/channel fast paths; ring data/space notifications; EOF/stop wakeups; correct priming for rings smaller than a sink block. |
| Runtime | Independent app/job tick deadlines account for elapsed work; input wakes the runtime after unlocking; hardware sources are sampled once per iteration. |
| Memory pressure | At most two broker compaction scans per pass, with recovery duration and compaction diagnostics. |

Each logical source change is a separate local commit. The branch history also
contains scoped test and contract documentation commits.

## Build and flash evidence

All four profiles built successfully from the same frozen firmware source.
Application flashes on the three connected boards passed esptool's written-data
hash verification. App-only final flashes preserved boot selection and storage.

| Profile | Final application SHA-256 | Device execution |
| --- | --- | --- |
| `solar_term` | `74e31682dea501a2fe31828584a4e4c7bb41f26099f5898658c98f0e1b7daa0e` | SolarTerm |
| `odroid_go` | `284794cfb21bfb8abdad42f4c560622be3c50423b0f2312673c1bf19f8443db8` | ODROID-GO, classic flavor |
| `seeed_reterminal_sticky` | `8a31ecad487952d6677abf5ff7432d86d23a5499f796b20aa9fc2fd302e24710` | reTerminal Sticky, SSD1677 |
| `esp32_s3_devkitc1_n16r8` | `f729a763d76c864a65fa2f10d643ed1286b433e2abe9b39fdd6a34a7afa376d6` | Build only |

Correctness and shared-download/storage stages were also flashed and exercised
before the final runtime/performance stage. Source, successful build, verified
flash, and runtime checks are separate evidence tiers.

## Checks

The combined host suite passed **233 tests**, plus the affected C checks for
storage, PCM, scheduling, job admission/lifetime and shell behavior. Coverage
includes real threaded mutex/parameter/adapter tests, forced losing mutex
candidates, ASan/UBSan, maximum domain lengths, redirect policy, fragmented and
oversized downloads, stored corruption, cancellation, failed syncs, grouped
rename failures, partial audio construction, stalled-sink cancellation, small
rings, mixed tick phases, clock wraparound and 64 registered reclaimers.

| Final device | HTTP cases | Parameter cycles | 1.5 MiB package + corrupt reinstall | PCM playback/cancel | Bindings | Recovery/completion |
| --- | --- | --- | --- | --- | --- | --- |
| SolarTerm | 5 passed | 8 per app, passed | Passed | 4 formats + cancel, passed | Python + Lua passed | Passed |
| ODROID-GO | 5 passed | Apps absent in classic | Passed | 4 formats + cancel, passed | Python passed; Lua absent | Passed |
| Sticky | 5 passed | 8 per app, passed | Passed | 4 formats + cancel, passed | Python + Lua passed | Passed |

The HTTP cases cover same-origin and cross-origin headers, HTTP 304, POST 303
conversion and POST 307 body preservation. Parameter reads overlap repeated
Synth/Funcgen destruction and recreation. WAV cases cover mono/stereo at
16/48 kHz. Download tests restore catalog/alias files and source/storage
settings; runtime tests remove their uniquely named fixtures and close owned
sessions. The pre-existing ODROID-GO flash-filesystem mount failure was not
formatted or repaired by these tests.

## Performance evidence and limits

Docs and Playground stage archives through a 4 KiB transfer allocation instead
of retaining an entire archive of up to 2 MiB. HTTP transport and crypto
allocations are additional.
With 64 reclaimers, the broker's own compaction scans fall from up to 66 to at
most two. Device diagnostics confirm the two-scan bound. PCM and ring tests
verify output/continuation and wakeup behavior; they do not establish a general
CPU or audio-quality improvement.

The finite foreground benchmark uses baseline `5a9ac9fd` and final source
`db580cd0`, with the same probe and device settings. Timing includes preemption.

| Device | Metric | Before (ms) | After (ms) |
| --- | --- | ---: | ---: |
| SolarTerm | Copy burst, 60 frames | 1014.057 | 962.054 |
| SolarTerm | Copy paced, 60 frames | 3164.467 | 3167.600 |
| SolarTerm | Exchange paced, 60 frames | 2790.634 | 2801.650 |
| Sticky | Copy burst, 60 frames | 1598.396 | 1518.202 |
| Sticky | Copy paced, 60 frames | 3723.953 | 3759.360 |
| Sticky | Exchange paced, 60 frames | 3188.955 | 3169.962 |

These are single before/after runs. Short compute timings vary and paced
SolarTerm rendering is essentially unchanged; no general throughput speedup is
claimed. Complete timing rows and raw evidence remain in
`/tmp/solar_os_4174_*`. The final matrix log is
`/tmp/solar_os_4174_final_matrix.log`.

Concurrent dual-panel attachment is tested with a mocked hardware provider,
not two physical panels. Silent PCM tests validate device execution and stream
release, not audible quality. These checks do not judge optical refresh quality.
Grouped file publication provides I/O error rollback, not a power-loss journal
or atomic visibility to concurrent readers. Legacy pause/cancel callbacks keep
bounded polling because they have no notification hook. Idle runtime waits
remain bounded to 25 ms, or 10 ms for boards requiring input polling.
