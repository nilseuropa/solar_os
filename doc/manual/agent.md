+++
id = "agent"
title = "Native SolarOS agent"
section = "app"
summary = "Configure and use the resumable LLM agent and its typed tools"
aliases = ["llm"]
keywords = "agent llm model openai responses reasoning tools policy prompt chat"
packages_any = ["app_agent"]
+++
# Native SolarOS agent

The `agent` application is a foreground chat that can inspect the current
device, consult this manual, edit files, and run installed Python or Lua
runtimes. It exposes only tools allowed by the current firmware and tool policy.

## Configure it

The Responses API is recommended for OpenAI reasoning models:

```text
agent config endpoint https://api.openai.com/v1/responses
agent config model gpt-model
agent config key api-key
agent config reasoning medium
agent config tools confirm
agent config max-tools 16
agent status
```

The default `confirm` policy runs read-only tools automatically and asks before
sensitive reads, file changes, or script execution. Use `readonly` when the
agent should only inspect the system.

## Start a conversation

Run `agent` or `agent new`, enter a message, and wait for the response. The
bottom bar shows
the latest input, output, and total token counts. Tool activity stays in the
conversation, while usage does not.

Use `agent --tts` for a spoken conversation, or add `--tts` before `new`,
`resume`, or `ask`. Start `speechd` with the desired PicoTTS voice first:

```text
job start speechd /voices/en-US
agent --tts
agent --tts ask What is the battery status?
```

The answer remains written in the terminal. Speech is queued only after a
successful final model answer. Connection status, retries, tool calls, tool
results, confirmation prompts, and intermediate model text preceding a tool
call remain silent. `--tts` currently selects the local PicoTTS speech service.

Completed turns are saved. Inspect and resume them explicitly:

```text
agent list
agent resume SLOT
agent delete SLOT
```

Slots are numbered `1` through `3` on internal flash or `1` through `8` on SD.
When all slots are occupied, a new conversation replaces the oldest one.
`agent resume` restores the transcript. Responses endpoints continue from the
saved provider response ID; Chat Completions endpoints receive a bounded local
message window. Bare `agent` always starts a new conversation.

Use `agent ask PROMPT` for one request. It leaves the answer visible until you
press `Esc` or the app-exit key; one-request answers are not added to the
conversation store.

## Inspect background workloads

The agent can inspect job admission without starting or stopping anything. For
example, ask:

```text
Which background jobs are running, and which stopped jobs could start now?
Why is email-sync waiting?
```

The answer is based on the same worker-stack admission policy used by SolarOS.
It includes current internal and PSRAM headroom, each job's declared stack
placement, generation, current resource claims, start disposition, and last
error. A `ready` disposition covers declared worker-stack admission only; a
job may still need dynamic buffers or an argument-selected resource when it
actually starts.

## Troubleshooting

- `agent status` shows the endpoint, model, policy, request failures, and memory
  telemetry without printing the API key. It also reports how many tool calls
  the last request used from its configured budget.
- If a generated script guesses an API or display name, ask the agent to call
  `solaros_reference` with the language and exact task, then call the relevant
  discovery tool. The reference lookup returns focused sections from the
  firmware's Python and Lua manuals rather than only their page summaries.
- If a call is denied, the model receives a structured denial and can explain
  the result or choose another approach.

## Quick reference

Use `agent` or `agent new` for a new durable foreground conversation. Add
`--tts` immediately after `agent` to speak final answers through a running
local `speechd`. `agent list` inspects saved slots, `agent resume SLOT` restores
one, and `agent delete SLOT` removes one. `agent ask PROMPT` makes an unsaved
one-shot request. Configure endpoint, model, key, reasoning, tool policy, and
maximum tools with `agent config`. `agent tools` reports the installed typed
tools and their policy disposition. `jobs_list` is read-only and reports actual
job admission and memory state; it does not start or stop jobs. The default
`confirm` policy runs read-only tools and requires local approval for sensitive,
mutating, or disruptive calls. `Esc` or the app-exit key returns to the shell.
