# Non-executable model interface fixture

`ports.tflite` is a generated TFL3 FlatBuffer containing a 32x32 RGB int8 input,
a ten-class output, one empty buffer and an `ethos-u` custom operator. It has no
accelerator command stream or learned weights and must never be sent to hardware.
Host tests use it with an emulated SSCMA chip to exercise programming, boundary
checks, cancellation, recovery and receipt handling without external repositories.
