# PicoTTS component

SolarOS builds the PicoTTS core from the pinned upstream revision documented
in [`../picotts.NOTICE`](../picotts.NOTICE). The local wrapper intentionally
does not compile a language into the application image.

During CMake configuration, all upstream languages are copied into
`<build>/picotts_voices/<locale>/`. Each runtime voice directory contains:

```text
ta.bin
sg.bin
```

Copy complete locale directories to SolarOS storage. Start the service with
`job start speechd <voice-directory>`. The buffers remain in PSRAM until the
job stops because PicoTTS reads its resources without copying them.
