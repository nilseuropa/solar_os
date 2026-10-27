#!/usr/bin/env python3
"""Apply/remove a strictly opt-in receiver overlay for the DLP experiment.

The generated patch is an experiment, not a production protocol allocation.
Only build with SOLAR_OS_DLP_LOSSLESS_TRIAL=1 in the environment to enable it. Both this
script and the patch remain available after restoring the production sources.
"""
import argparse
import difflib
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
FILES = ("protocol/display_link/include/sdlp.h", "protocol/display_link/src/sdlp.c", "src/CMakeLists.txt")


def replace(text, old, new):
    assert text.count(old) == 1, f"Expected one source match: {old!r}"
    return text.replace(old, new)


def overlay(name, text):
    if name.endswith("CMakeLists.txt"):
        return text + '''
# Local DLP lossless experiment; no effect in ordinary firmware builds.
if(DEFINED ENV{SOLAR_OS_DLP_LOSSLESS_TRIAL} AND "$ENV{SOLAR_OS_DLP_LOSSLESS_TRIAL}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SOLAR_OS_DLP_LOSSLESS_TRIAL=1)
endif()
'''
    if name.endswith(".h"):
        return replace(text, "  uint8_t decoded[SDLP_RLE_MAX_DECODED];", """  uint8_t decoded[SDLP_RLE_MAX_DECODED];
#ifdef SOLAR_OS_DLP_LOSSLESS_TRIAL
  uint32_t trial_hash, trial_bytes;
#endif""")
    text = replace(text, "#include <string.h>", """#include <string.h>
#ifdef SOLAR_OS_DLP_LOSSLESS_TRIAL
#include "../../../experiments/dlp_lossless/decoder.h"
#endif
static bool trial_rectangle(unsigned op) {
#ifdef SOLAR_OS_DLP_LOSSLESS_TRIAL
  return op == 0x16;
#else
  (void)op;
  return false;
#endif
}""")
    text = replace(text, "  if (q->opcode == SDLP_GET_EXTENSIONS || q->opcode == SDLP_GET_SYNC) {", """#ifdef SOLAR_OS_DLP_LOSSLESS_TRIAL
  /* Private experiment query: version, scratch limit, decoded-stream FNV-1a,
   * byte count. This opcode is not advertised as a production extension. */
  if (q->opcode == 8) {
    if (q->length) return SDLP_BAD_ARGUMENT;
    sdlp_put_u16(a->payload, 1);
    sdlp_put_u16(a->payload + 2, sizeof(d->decoded));
    sdlp_put_u32(a->payload + 4, d->trial_hash);
    sdlp_put_u32(a->payload + 8, d->trial_bytes);
    a->length = 12;
    return SDLP_OK;
  }
#endif
  if (q->opcode == SDLP_GET_EXTENSIONS || q->opcode == SDLP_GET_SYNC) {""")
    text = replace(text,
        "q->opcode != SDLP_RECT_DATA && q->opcode != SDLP_RECT_RLE && q->opcode != SDLP_PRESENT &&",
        "q->opcode != SDLP_RECT_DATA && q->opcode != SDLP_RECT_RLE && !trial_rectangle(q->opcode) && q->opcode != SDLP_PRESENT &&")
    text = replace(text,
        "q.opcode != SDLP_RECT_DATA && q.opcode != SDLP_RECT_RLE &&",
        "q.opcode != SDLP_RECT_DATA && q.opcode != SDLP_RECT_RLE && !trial_rectangle(q.opcode) &&")
    text = replace(text, "    d->have_rect = false;\n    return SDLP_OK;", """    d->have_rect = false;
#ifdef SOLAR_OS_DLP_LOSSLESS_TRIAL
    d->trial_hash = UINT32_C(2166136261);
    d->trial_bytes = 0;
#endif
    return SDLP_OK;""")
    text = replace(text,
        "  } else if (q->opcode == SDLP_RECT_DATA || q->opcode == SDLP_RECT_RLE) {",
        "  } else if (q->opcode == SDLP_RECT_DATA || q->opcode == SDLP_RECT_RLE || trial_rectangle(q->opcode)) {")
    text = replace(text, """    if (q->opcode == SDLP_RECT_RLE) {
      if (!(d->info.capabilities & SDLP_RLE)) return SDLP_UNSUPPORTED;""", """    if (q->opcode == SDLP_RECT_RLE || trial_rectangle(q->opcode)) {
      if (q->opcode == SDLP_RECT_RLE && !(d->info.capabilities & SDLP_RLE)) return SDLP_UNSUPPORTED;""")
    text = replace(text, """      if (!n || n > sizeof(d->decoded) ||
          !sdlp_rle_decode(d->decoded, n, p + 14, q->length - 14))
        return SDLP_BAD_ARGUMENT;""", """      if (!n || n > sizeof(d->decoded)) return SDLP_BAD_ARGUMENT;
      bool decoded;
#ifdef SOLAR_OS_DLP_LOSSLESS_TRIAL
      if (trial_rectangle(q->opcode))
        decoded = solar_os_dlp_trial_lz4_decode(d->decoded, n, p + 14, q->length - 14);
      else
#endif
        decoded = sdlp_rle_decode(d->decoded, n, p + 14, q->length - 14);
      if (!decoded) return SDLP_BAD_ARGUMENT;""")
    return replace(text, """    r = d->backend.write(ctx, s->offset, bytes, n);
    if (!r) {""", """    r = d->backend.write(ctx, s->offset, bytes, n);
    if (!r) {
#ifdef SOLAR_OS_DLP_LOSSLESS_TRIAL
      for (uint32_t k = 0; k < n; k++)
        d->trial_hash = (d->trial_hash ^ bytes[k]) * UINT32_C(16777619);
      d->trial_bytes += n;
#endif""")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("apply", "restore"))
    args = parser.parse_args()
    state = HERE / ".overlay-state.json"
    if args.action == "restore":
        saved = json.loads(state.read_text())
        for name, item in saved.items():
            assert (ROOT / name).read_text() == item["modified"], f"Concurrent edit: {name}"
        for name, item in saved.items():
            (ROOT / name).write_text(item["original"])
        state.unlink()
        return
    assert not state.exists(), "Overlay already applied"
    saved = {}
    patch = []
    for name in FILES:
        original = (ROOT / name).read_text()
        modified = overlay(name, original)
        saved[name] = {"original": original, "modified": modified}
        patch.extend(difflib.unified_diff(original.splitlines(True), modified.splitlines(True),
                                        fromfile="a/" + name, tofile="b/" + name))
    (HERE / "receiver.patch").write_text("".join(patch))
    state.write_text(json.dumps(saved))
    for name, item in saved.items():
        (ROOT / name).write_text(item["modified"])


if __name__ == "__main__":
    main()
