+++
id = "jobs"
title = "Background jobs"
section = "job"
summary = "Inspect and control bounded background workers"
aliases = ["job", "background"]
keywords = "python lua jobs background task process start stop state waiting failed memory stack"
packages_any = []
+++
# Background jobs

Jobs are named background workers such as logging, acquisition, bridges, NTP,
and HTTP serving. They are not foreground applications and continue when you
switch shell or app sessions.

## Inspect memory and ownership

Run `jobs` for the compact overview and `job status NAME` for details:

```text
jobs
job status log
```

The status includes the worker stack size and whether it uses internal RAM or
PSRAM. Detailed status also shows the current start-admission disposition and
reason using the same centralized policy that launches jobs. It shows claimed
resources and the last error. Check `mem` before starting several
internal-stack jobs together.

## Waiting and failed

`waiting` means the requested worker has not passed launch admission yet. It
may run later when the required memory or resource becomes available.

`failed` means launch or runtime work ended with an error. Inspect `last_error`
and the resource owner before retrying.

## Start and stop

```text
job start batmon 60
job start log file /logs/system.log info
job start midi midi0
job stop log
```

Only one instance of each registered job name is active. Starting it again
replaces the previous invocation.

## Quick reference

solaros.jobs provides list(), count(), status(name), start(name,
optional_args), and stop(name). Status includes state, last_error,
worker_stack_bytes, worker_stack_external, tick timing, and deadline telemetry.
The shell's detailed status adds the current start-admission disposition and
reason.
Waiting means launch admission has not yet succeeded; failed records a terminal
launch or runtime error.

The `midi` job owns one named MIDI bus and moves messages in both directions.
Create the bus with `expansion bus create midi`, then use `midi status` to
inspect its counters. `midi stream add <channel> <controller>` exposes an exact
incoming CC as a scalar `midi.cc.<channel>.<controller>` stream.

The `controls` job samples configured scalar streams at 50 Hz and sends changed
normalized values to native application parameters and MIDI CC targets. Create
the mappings with `control`; see `man controls` for the complete workflow.
