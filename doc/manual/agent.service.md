+++
id = "agent.service"
title = "Agent service and tool reference"
section = "app"
summary = "Provider contract, typed tools, policy, resource bounds, and roadmap"
aliases = ["agent.tools"]
keywords = "agent service responses api tools policy memory bounds provider"
packages_any = ["app_agent"]
+++
# Native Agent Service

`service.agent` is the native control plane for connecting SolarOS to remote
language models. `app.agent` is a resumable foreground chat frontend for
display and port shells.

The first provider supports both the OpenAI Responses API and compatible Chat
Completions endpoints. Configure the complete endpoint URL rather than a
provider base URL. Responses is recommended for OpenAI reasoning models:

```text
agent config endpoint https://api.openai.com/v1/responses
agent config model gpt-model
agent config key api-key
agent config reasoning medium
agent config tools confirm
agent config max-tools 16
agent status
```

The endpoint path selects the wire protocol: URLs ending in `/responses` use
typed Responses events, while other URLs use Chat Completions. Reasoning effort
may be `none`, `minimal`, `low`, `medium`, `high`, `xhigh`, or `max`; support
still depends on the selected model. The default is `medium`.

Responses tool continuations use `previous_response_id`, so reasoning context
is retained without copying reasoning tokens through the device. Responses are
stored by the provider for that request chain. Instructions are sent again on
the continuation because Responses does not carry previous instructions
forward automatically.

For the official OpenAI Chat Completions endpoint, the adapter instead sends
`reasoning_effort: none` so function tools remain compatible with that API.
Other Chat-compatible endpoints do not receive this provider-specific field.

Configuration is stored in the `agent` NVS namespace. The API key is used as a
Bearer token and is never returned by status. Use `agent config key clear` for
a local or self-hosted endpoint without authentication, or `agent forget` to
erase all agent configuration.

Start the foreground chat with:

```text
agent
agent new
```

Enter sends a message, Page Up/Down scrolls the terminal transcript, and `Esc`
or the app-exit key returns to the shell. The prompt uses the configured SolarOS
username, agent labels are bold, and the protected bottom status bar shows the
latest input, output, and total token counts without adding usage lines to the
conversation or scrollback. Narrow displays abbreviate those fields as `I`, `O`,
and `T`. Completed turns are stored under `.solar/agent/conversations` on the
preferred persistent filesystem. Use `agent list`, `agent resume SLOT`, and
`agent delete SLOT` to manage them. New conversations use slots `1` through `3`
on internal flash or `1` through `8` on SD; once full, the oldest slot is
replaced atomically. Exiting and launching bare `agent` again
still starts a new conversation; restoring an old one is always explicit.

Each provider adapter declares its resume mode. Responses resumes from the
saved provider response ID. Chat Completions rebuilds a bounded request from
normalized local user and assistant messages. Restoring the visible transcript
streams checked records from storage rather than placing the entire
conversation in internal SRAM.

Only completed turns are committed. Each conversation is an independently
checksummed file replaced through temporary and backup names, so an interrupted
write cannot damage other conversations. With internal flash selected, the
store keeps at most three 10 KiB conversations; with SD selected, it keeps at
most eight 48 KiB conversations. Oldest conversations are evicted
deterministically. Provider, model, title, timestamps, messages, tool summaries,
and continuation IDs are stored; the bearer key remains only in NVS.

For a single request, use:

```text
agent ask Describe the current device status.
```

The one-request frontend stays open after completion so the response remains
readable. It is not persisted. Use `Esc` or the app-exit key to return to the
shell.

On full builds, the same 16 KiB foreground worker can run a Python or Lua
source string or file through the reusable script-runner contract:

```text
agent script python -c "print(6 * 7)"
agent script lua -c "print(6 * 7)"
agent script python /script.py argument
agent script lua /script.lua argument
```

This manual path captures output instead of streaming directly from the
interpreter. Output is limited to 4095 bytes and execution to 30 seconds. The
`Esc` or the app-exit key cancels a running script. Python and Lua each have a
single-owner guard, so a captured run cannot overlap their foreground app or
REPL.
Exceptions, cancellation, deadlines, truncation, and completion are returned
as structured runner status.

The adapter requests server-sent streaming events and emits provider-neutral
events for text deltas, usage, tool calls, errors, and completion. Cancellation
uses the shared HTTP client's cross-task cancellation path. TLS certificate
validation uses the firmware certificate bundle. A stateless local provider
turn that contains neither text nor a tool call is retried once; a second empty
turn is reported as an invalid response instead of silently returning to the
prompt.

## Typed tools

`agent tools` shows the canonical registry, domain and risk metadata, runtime
availability, current policy disposition, and any required capability. The
registry can be larger than the schema set sent to the provider. Every request
starts with only three bootstrap tools:

- `system_status`: board ID, SolarOS version, uptime, free and largest internal
  RAM blocks, and free PSRAM.
- `solaros_reference`: search the same package-aware manual source exposed to
  users by `man -k` and `man TOPIC`. The tool returns up to three matching
  topics' Quick references from the signed downloaded manual. Without that
  manual, it returns the built-in setup references or download instructions. It
  accepts exactly one `query` field combining the language and
  task, such as `{"query":"lua gfx drawing"}`. Every result includes mandatory
  SolarOS coding guidance: use documented constants rather than guessed
  strings or numbers, discover hardware names, respect package gates, and
  preserve cleanup patterns. Search uses topic names, aliases, summaries,
  keywords, and Quick references. Downloaded graphics references include
  copyable Python and Lua setup/cleanup skeletons using `gfx.WHITE`, `gfx.BLACK`,
  a verified target, and the language-correct `gfx.end()` or `gfx["end"]()`.
  Install or refresh the manual with `help update` before using APIs whose
  topics return download instructions. A retained manual from an earlier
  firmware version is marked as potentially outdated by `help status`.
- `tool_search`: find and activate up to five installed tools relevant to an
  exact task. A later search replaces the previous dynamic selection while
  retaining the three bootstrap tools, so no provider turn advertises more
  than eight schemas.

Selected workflow dependencies such as `script_run_file` may activate on
demand when they were not selected for the current request. SolarOS advertises
the schema and asks the provider to retry. The first call is not executed and
cannot bypass normal confirmation. Other inactive tools still require
`tool_search`, preventing a model from activating an unsuitable operation by
guessing its name.

The package- and policy-gated tools discoverable through `tool_search` are:

- `storage_list`: up to 16 file or directory entries for one path, including
  type and size. Results report when the output was truncated.
- `storage_stat`: check one exact relative or absolute path and return whether
  it exists, its resolved path, type, and size. This is the required operation
  for file-existence questions. Prompts containing a path-like token such as
  `mandel.py` or `/config.lua` select this schema before fuzzy tool matching.
- `storage_read`: read up to 3072 bytes from a text-file path. This is a
  sensitive read and requires confirmation under the default policy.
- `storage_write`: create or replace a text-file path with up to 3072 bytes.
  This is mutating and requires confirmation by default.
  Relative storage paths use the invoking shell's current directory; absolute
  paths retain their normal SolarOS storage semantics.
  All storage content and editing tools reject paths below `.ssh`, so the agent
  cannot inspect or replace the device's SSH identity files.
- `storage_search`: search file contents under one file or a bounded directory
  tree, returning paths, line numbers, and excerpts. It does not search file
  names or test exact-path existence. It scans at most 32 paths, 64 KiB, and
  12 matches per call.
- `storage_read_range`: read up to 2048 bytes at a byte offset and return the
  complete file's SHA-256, next offset, and end-of-file state.
- `storage_patch`: apply up to eight ascending, non-overlapping byte-offset
  edits to a file no larger than 128 KiB. The caller must present the SHA-256
  returned by `storage_read_range`; a stale version returns a structured
  conflict without changing the file. Successful patches are staged on the
  same volume; the verified original is retained as a backup until the staged
  file has acquired the public path. If the volume has no spare allocation
  unit for staging, replacement uses the PSRAM copy and restores the original
  on a write failure.
- `jobs_list`: read-only workload inspection using the actual centralized task
  admission policy. An empty `name` lists jobs; a job name requests one complete
  record. Results include a point-in-time internal/PSRAM free and largest-block
  snapshot, the background reserve and task overhead, plus each job's state,
  generation, `ready`/`running`/`waiting_for_memory`/`blocked` start
  disposition, machine-readable reason, last error, declared worker-stack
  placement, owner, and current resource claims. It never starts or stops a
  job. Resource claims describe the current invocation, and `ready` cannot
  predict undeclared dynamic buffers or argument-selected resources.
- `display_list`: registered display targets with their real names, drivers,
  dimensions, readiness, roles, brightness support, and current owners. The
  provider must call this before generating code for an attached display and
  use only a returned ready target.
- `hardware_describe`: compiled board identity, capabilities, and installed
  PSRAM. This is the first check before assuming a peripheral exists.
- `gpio_list`: real board GPIO slots, pin policy, availability, claims,
  configuration, and already-readable levels without configuring a pin.
- `gpio_read`: inspect one real board GPIO slot. It returns a level only when
  the pin is already configured and readable; it never claims or configures the
  pin as a side effect.
- `buses_list`: registered I2C, SPI, UART, MIDI, OneWire, and PS/2 buses with their actual
  names, pin configuration, readiness, origin, sharing mode, and lease count.
- `network_status`: current Wi-Fi station, IP, access-point, signal, channel,
  and NAT state without changing the provider connection.
- `sensors_read`: battery and environmental readings from installed service
  packages. A family absent from the build is returned as unavailable.
- `script_run_python` and `script_run_lua`: execute a source string through the
  installed interpreter adapter. Generated scripts have access to the same
  SolarOS APIs as local scripts, so both tools are classified as disruptive.
- `script_run_file`: execute a saved Python or Lua file with up to seven
  arguments through that same runner, including captured output, structured
  errors, cancellation, ownership guards, and the 30-second deadline.

Tool policy is NVS-backed and enforced again inside the canonical executor:

| Policy | Behavior |
| --- | --- |
| `off` | Advertise and execute no tools. |
| `readonly` | Advertise and execute only read-only tools. Sensitive reads are excluded. |
| `confirm` | Run read-only tools automatically and require one local confirmation for every sensitive, mutating, or disruptive call. This is the default. |
| `all` | Run every available tool without confirmation. This must be selected explicitly. |

Under `confirm`, the foreground app prints the exact bounded JSON arguments and
waits up to 30 seconds at `Allow once? [y/N]`. Only `y` allows that call;
`n`, Enter, or the timeout denies it. `Esc` or the app-exit key cancels the whole
request. A denial is returned to the model as a structured result so it can
explain or choose another approach rather than losing the conversation turn.
`agent status` includes executed, denied, and failed tool counters.

The service permits up to 16 sequential tool calls by default and always
reserves a separate provider turn for the final response. Set the per-request
limit with `agent config max-tools COUNT`; accepted values are 1 through 32 and
the selection is stored in NVS. Once the budget is consumed, the reserved final
turn advertises no tools, so the model must conclude from the results already
collected instead of failing by requesting another tool. This supports bounded
inspect/read/change/test/repair workflows without allowing an unbounded
autonomous loop. Unsupported tools, multiple simultaneous tool calls, and
malformed arguments still fail the request.
Definitions, input/output schemas, availability checks, risk metadata, and
executors live in one declarative registry. Only available tools are sent to
the provider. Discovery also filters tools denied by the current policy. The
agent rejects calls to tools outside the currently advertised set, and every
successful executor result is parsed as a JSON object before it is returned to
the model.

## Resource bounds

- Foreground worker stack: 16 KiB internal RAM.
- Stream-line buffer: 24 KiB, PSRAM preferred. Responses completion events
  contain the assembled output as one event.
- Request body: 16 KiB, PSRAM preferred.
- Prompt: 1023 bytes.
- Tool discovery query: 159 bytes.
- Tool descriptor buffer: 6 KiB, allocated in PSRAM. At most three bootstrap
  plus five discovered tool schemas are serialized per provider turn.
- Tool arguments: 4095 bytes, held in PSRAM for a request.
- Tool result: 4095 bytes, allocated in PSRAM.
- API-reference matches: at most three topic Quick references per lookup,
  subject to the 4095-byte tool-result limit. Firmware does not embed full
  scripting guides or a scripting-section excerpt index.
- Manual search: fixed stack storage for at most 12 results. Built-in setup
  text and download notices remain in flash. Full verified SD pages are loaded
  into PSRAM while being read; the signed catalog supplies Quick references
  for the agent.
- Storage read/write content: 3072 bytes.
- Storage ranged read: 2048 requested bytes; JSON output may stop earlier when
  escaping would exhaust the bounded tool result.
- Storage search: 32 queued paths, 64 KiB scanned text, and 12 matches.
- Storage patch: 128 KiB file, eight edits, and 2048 inserted bytes per edit;
  file and edit buffers require PSRAM.
- Generated script source: 640 bytes.
- Generated script captured output: 383 bytes.
- Tool confirmation deadline: 30 seconds.
- Manual script output: 4095 bytes, allocated in PSRAM.
- Manual script deadline: 30 seconds.
- Model output: 16 KiB per provider turn.
- Provider request body: 32 KiB in PSRAM, including at most 8 KiB of raw local
  history serialized into a 16 KiB history fragment for Chat Completions.
- Assistant capture for a completed turn: 16 KiB in PSRAM.
- Conversation file: 10 KiB on internal flash or 48 KiB on SD; three flash
  conversations or eight SD conversations are retained.
- Request deadline: 90 seconds.
- Per-I/O timeout: 15 seconds.

`agent status` records tool calls used versus the last request's configured
budget, internal free memory, the lowest sample observed during HTTP streaming,
largest internal blocks, and PSRAM before and at request completion. The
completion sample still includes the foreground worker stack; run `mem policy`
after the app returns to confirm that the task stack was reclaimed.

The agent package requires Wi-Fi and PSRAM but does not require Python or Lua.
The manual command and model tool are advertised only when the corresponding
runtime is in the firmware; the agent app supplies the optional adapter callback
without making either interpreter a package dependency. Larger-file editing,
mutating job/network/hardware operations, additional providers, remote
interfaces, and scheduling are later phases.

## Quick reference

The native agent uses the configured OpenAI-compatible Responses endpoint,
applies the selected confirmation policy, and exposes bounded typed tools for
installed SolarOS features. Completed chat turns are stored locally;
`agent list`, `agent resume SLOT`, and `agent delete SLOT` manage them. Responses
uses a provider continuation ID and Chat Completions uses bounded local
history. `agent status` reports provider, policy, requests, tool activity, and
memory. This page defines the tool contract and resource limits; `man agent` is
the task-oriented usage guide.
