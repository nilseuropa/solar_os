# PicoTTS component

SolarOS builds the PicoTTS core from the pinned upstream revision documented
in [`../picotts.NOTICE`](../picotts.NOTICE). The local wrapper intentionally
does not compile a language into the application image.

Versioned language resources live in the repository's top-level
[`picotts_voices`](../../picotts_voices) directory. During CMake configuration,
they are also copied into `<build>/picotts_voices/<locale>/`. Each runtime
voice directory contains:

```text
ta.bin
sg.bin
```

Copy a complete locale directory from the repository to SolarOS storage. Start
the service with `job start speechd <voice-directory>`. The buffers remain in
PSRAM until the job stops because PicoTTS reads its resources without copying
them.
