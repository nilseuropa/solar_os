+++
id = "lua.agent"
title = "Lua agent API"
section = "api"
summary = "Ask language models about text or images, configure providers, and resume conversations"
keywords = "lua solaros agent ask image camera vision provider conversation tools"
packages_any = ["service_agent"]
agent_reference_sections = true
+++
# Lua agent API

[Lua API overview](lua.md) · [Python agent API](python.agent.md) · [Agent service](agent.service.md)

## `solaros.agent`

```lua
local solaros = require("solaros")

if solaros.agent then
    local answer = solaros.agent.ask("Describe the image.", "stream:camera0")
    print(answer.text)
end
```

The module requires the interpreter and `service.agent`. The device needs Wi-Fi,
PSRAM, a configured provider endpoint and model, and credentials if the provider
requires them. Check module availability before calling it. Configuration is
shared with the shell Agent app and saved in NVS; changes affect later requests.

## Requests and images

`ask(prompt[, images[, conversation_id]])` blocks until completion and returns
`text`, `conversation_id`, `truncated`, `prompt_tokens`,
`completion_tokens`, and `total_tokens`. Usage describes the latest provider
turn; providers that do not report it leave zeroes. Text is bounded to 16 KiB,
with `truncated` indicating omitted text. The prompt limit is 1023 bytes.

Omit `images` for a text question. Pass one source string or a sequence of up
to two strings for an image question. Sources are SolarOS file paths,
`stream:ID`, `rtsp://URL`, or `pipeline:ID`; use actual discovered stream
names or pipeline IDs. Files use the normal scripting storage path rules.
Camera/stream, RTSP, and pipeline sources require their respective packages
and an available source. Each source read waits up to five seconds.

Attachments must be encoded JPEG, PNG, GIF, or WebP, totalling at most 512 KiB.
They are sent to the model as image content, without local decoding or
transcoding. Select a provider model that accepts images. Samples and temporary
source ownership are released on success, cancellation, and failure. A saved
conversation records an attachment marker locally; image bytes are not saved.
See [Agent image questions](agent.md#ask-about-an-image) for provider history behavior.

## Conversations

The default request is stateless and returns an empty `conversation_id`.
Pass an empty string as the third argument to create a saved conversation.
Pass a returned ID to resume it; this is an ID, rather than a numeric shell slot.
Completed turns are saved through the same storage service as the Agent app.

- `conversations()` lists up to eight saved conversation records.
- `conversation_info(id)` returns one record.
- `delete_conversation(id)` deletes one saved conversation.

Records contain `id`, `title`, `provider`, `model`, `created_at`,
`updated_at`, `turn_count`, and `stored_bytes`. Timestamps are native
service timestamps.

## Configuration and status

- `status()` returns configuration and request diagnostics. Fields include
  `initialized`, `configured`, `running`, `provider`, `endpoint`,
  `model`, `reasoning_effort`, `tool_policy`, `max_tools`, `api_key_set`,
  request/failure/tool counters, last HTTP status, error, duration, received
  bytes, and internal/PSRAM memory diagnostics. It never returns the API key.
- `set_endpoint(url)`, `set_model(name)`, `set_api_key(key)`, and
  `set_reasoning_effort(effort)` use the native configuration validation.
  An empty API key clears authentication.
- `set_tool_policy(name)` accepts `off`, `readonly`, `confirm`, or `all`.
- `set_max_tools(count)` accepts an integer from 1 through 32.
- `forget()` erases agent configuration; it leaves saved conversations intact.
- `cancel()` requests cancellation of the current agent request, including a
  request started by another frontend. It is harmless when idle.

## Ownership, policy, and errors

Only one agent request can run at a time. Calls from an already active
agent-owned script fail with `ESP_ERR_INVALID_STATE`. Agent requests made
from scripts do not advertise or execute Python/Lua script tools, so they
cannot re-enter either interpreter.

The configured tool policy remains in force. Calls requiring confirmation are
denied because these bindings have no confirmation UI. Read-only tools remain
available under `confirm`; `off` disables tools. Selecting `all` explicitly
allows the other installed native tools without confirmation.

Interpreter exit and captured-run cancellation/deadlines propagate to image
reads and provider HTTP polling. Service failures raise the language's normal
SolarOS error, including provider rejection, missing sources, unavailable
packages, memory exhaustion, and cancellation. Invalid image sequences or
out-of-range tool limits raise argument errors.

Lua uses `nil` for omitted images or conversations and an array table for
multiple image sources. Service errors raise a Lua error.

```lua
local solaros = require("solaros")

local first = solaros.agent.ask("Compare these pictures.", {"/first.jpg", "/second.png"}, "")
local followup = solaros.agent.ask("Summarize your answer.", nil, first.conversation_id)
print(followup.text)
```

## Quick reference

Use `solaros.agent.ask(prompt[, images[, conversation_id]])`. Images are one
source string or an array table of up to two strings: file paths, stream:ID,
rtsp://URL, or pipeline:ID. The returned table contains text, conversation_id,
truncated, prompt_tokens, completion_tokens, total_tokens. An empty conversation
ID creates saved history; omit it for a stateless request. Requires Lua and
service.agent; ask cannot execute script tools. Configuration uses set_endpoint,
set_model, set_api_key, set_reasoning_effort, set_tool_policy, set_max_tools.
Use status, conversations, conversation_info, delete_conversation, cancel, and
forget for management. Never embed credentials in shared scripts.
