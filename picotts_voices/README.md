# PicoTTS voices

These runtime voice resources come from
[`DiUS/esp-picotts`](https://github.com/DiUS/esp-picotts) commit
`bf1a8df9d2be1e088a03a775d439ac66e18438dc`. Their license and attribution are
recorded in [`components/picotts.NOTICE`](../components/picotts.NOTICE).

Each locale directory is ready to copy to SolarOS storage and contains the two
filenames expected by `speechd`:

```text
ta.bin
sg.bin
```

For example, copy `picotts_voices/de-DE/` to a device as `/voices/de-DE/`, then
select it at runtime:

```text
job start speechd /voices/de-DE
say "Solar O S ist bereit"
```

Stop and restart `speechd` with another locale directory to change languages.
The files remain external to `firmware.bin`.
