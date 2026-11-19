+++
id = "agent"
title = "Native SolarOS agent"
section = "app"
summary = "Ask questions with text and images, hear synthesized answers, and use the agent's typed tools"
aliases = ["llm"]
keywords = "agent llm model openai responses reasoning tools policy prompt chat image camera multimodal speech synthesis tts picotts voice python lua audio wav mp3 ask_audio"
packages_any = ["app_agent"]
+++
# Native SolarOS agent

The `agent` application is a foreground chat that can inspect the current
device, consult this manual, edit files, and run installed Python or Lua
runtimes. It exposes only tools allowed by the current firmware and tool policy.
Questions can include image attachments. Add `--tts` to hear final answers
through local speech synthesis while keeping their text in the terminal.

## Configure it

Set your provider endpoint, model name, and API key. For a Responses endpoint:

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
bottom bar shows the latest input, output, and total token counts. Tool
activity stays in the conversation, while usage does not.

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

Use `agent ask PROMPT` for one foreground request. One-request answers are not
added to the conversation store. `Esc` or the app-exit key cancels an active
request.

## Speech output

`agent --tts` speaks successful final answers through the device's audio
output. You enter questions as text or attach images; speech is synthesized
locally from the model's written answer. This uses the optional `speech`
package and PicoTTS. The remote provider supplies text and does not need an
audio output model.

Copy a complete PicoTTS voice folder, including `ta.bin` and `sg.bin`, to SD
or flash storage. Ready-to-copy locale folders are supplied in
`picotts_voices/` and mirrored under `.pio/build/<environment>/picotts_voices/`
after a firmware build. For example, copy `en-US` to `/voices/en-US`, then
start the speech job and check playback:

```text
job start speechd /voices/en-US
say "Solar O S is ready"
agent --tts
```

Place `--tts` immediately after `agent`. It applies to the current launch and
works with new chats, saved conversations, one-request questions, and images:

```text
agent --tts new
agent --tts resume SLOT
agent --tts ask What is the battery status?
agent --tts ask --image stream:camera0 Describe what the camera sees.
```

The answer remains written in the terminal. Speech is queued only after a
successful final model answer. Connection status, retries, tool calls, tool
results, confirmation prompts, and intermediate model text preceding a tool
call remain silent. `--tts` currently selects the local PicoTTS speech service.
Playback uses the global speaker level, set with `audio level VOLUME`.
Other audio users can delay queued speech. Long answers are split into chunks;
limited queue space can shorten the spoken portion while preserving the full
written answer.

Restart `speechd` with another voice directory to change language. Inspect its
state with `job status speechd`; `job stop speechd` cancels pending speech and
releases the voice resources. See [speechd](jobs.reference.md#speechd) for voice
languages, storage requirements, and queue behavior.

## Ask about an image

Select a vision-capable model, then attach a saved picture, a single camera or
RTSP frame, or the image matching a pipeline's latest result:

```text
agent ask --image /pictures/sample.jpg What is in this picture?
agent ask --image stream:camera0 Describe what the camera sees.
agent ask --image rtsp://192.168.1.20/media What is happening?
agent ask --image pipeline:1 Describe this processed frame.
agent ask --image /pictures/before.png --image /pictures/after.png What changed?
```

In an interactive conversation, use `/image SOURCE QUESTION`, or the same
leading `--image SOURCE` options. Quote paths containing spaces. Options must
precede the question; `--` ends the image options when the question itself
begins with `--image`. Image paths complete in the shell.

Up to two JPEG, PNG, GIF, or WebP attachments may total 512 KiB of encoded
data. Bytes are sent unchanged as data URLs; the device does not decode or
resize them. The configured provider/model must support images, including the
image format supplied. Provider errors are shown without a text-only retry.
Image uploads work with Responses and compatible Chat Completions endpoints.

`stream:` captures once using the source's default mode. `rtsp://` selects one
JPEG video frame, with a five-second receive deadline and no audio playback.
The source and its hardware lease are released before upload. A camera already
owned by Photo, `rtspd`, or another job reports busy. `pipeline:` requires the
pipeline service and an existing completed result; it uses that result's
retained encoded image without opening its camera again. Pipeline observations
remain available through `pipeline result`; they are not attached automatically.

Images belong to the current request, including its tool continuations.
Saved conversation transcripts record an attachment notice and omit image
bytes. Chat Completions resumes therefore include the notice without the old
images; attach them again for another visual comparison. Responses resumes
use the saved provider response ID and the provider's retained context.

## Questions from scripts

With `service.agent` and Python or Lua included, scripts can submit text and
image questions through `solaros.agent.ask(prompt[, images[, conversation_id]])`.
Use the same provider configuration as the Agent app. One source string or a
sequence of up to two sources supplies the images.

For a stored audio recording, use
`solaros.agent.ask_audio(prompt, path[, conversation_id])`. It accepts one
encoded WAV or MP3 file up to 1 MiB and returns a written answer. Select an
audio-capable model and a Chat Completions endpoint; the SolarOS Responses
adapter rejects audio input. Submitting a file needs no microphone or local
audio package.

```python
import solaros

answer = solaros.agent.ask_audio("Summarize this recording.", "/recording.wav")
print(answer["text"])
```

See the [Python agent API](python.agent.md) and [Lua agent API](lua.agent.md)
for image examples, saved conversations, cancellation, and tool policy.

## Inspect background workloads

The agent can inspect job admission without starting or stopping anything. For
example, ask:

```text
Which background jobs are running, and which stopped jobs could start now?
Why is email-sync waiting?
```

The answer reports memory, resource owners, and job errors. A job shown as
`ready` has enough declared worker-stack memory at that moment; it may still
need more buffers or an available resource when it starts.

## Troubleshooting

- If `--tts` reports `speechd is not running`, start the job with a valid voice
  directory. `TTS is unavailable in this firmware` requires a build with the
  `speech` package and a working audio output.
- If the answer is visible but silent, check `job status speechd`, test `say`,
  and inspect `audio level`. A full queue or a stopped speech job is reported
  in the Agent terminal; the written answer remains available.
- `agent status` shows the endpoint, model, policy, request failures, and memory
  telemetry without printing the API key. It also reports how many tool calls
  the last request used from its configured budget.
- If a generated script guesses an API or display name, ask the agent to call
  `solaros_reference` with the language and exact task, then call the relevant
  discovery tool. The reference lookup returns Quick references from the
  signed downloaded manual. If a topic directs you to install the manual,
  run `help update` before using its APIs.
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

`agent ask --image SOURCE QUESTION` attaches a file, `stream:ID`, `rtsp://` JPEG
frame, or `pipeline:ID` image. In chat, use `/image SOURCE QUESTION`. Up to two
images may total 512 KiB; select a vision-capable provider/model. Speech output
can be combined with these image requests using `agent --tts ask --image ...`.
Python and Lua scripts use `solaros.agent.ask()` for text/images and
`solaros.agent.ask_audio(prompt, path[, conversation_id])` for one WAV/MP3 file
up to 1 MiB with an audio-capable Chat Completions model.
