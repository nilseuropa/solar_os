#!/usr/bin/env python3
"""Prepare isolated Pico RAM baseline/trial source copies; never edit the source repo."""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

from trial_overlay import overlay, replace

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=HERE.parents[2] / "pico_display_link")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    snapshot = args.output / "baseline"
    snapshot.mkdir()
    for name in ("src", "protocol"):
        shutil.copytree(args.source / name, snapshot / name,
                        ignore=shutil.ignore_patterns("vendor", "__pycache__", "*.o"))
    shutil.copy2(args.source / "CMakeLists.txt", snapshot / "CMakeLists.txt")
    trial = args.output / "trial"
    shutil.copytree(snapshot, trial)
    changes = {}
    for name in ("protocol/include/sdlp.h", "protocol/src/sdlp.c"):
        path = trial / name
        original = path.read_text()
        modified = overlay(name, original)
        if name.endswith(".h"):
            modified = replace(modified, "  uint32_t trial_hash, trial_bytes;", """  uint32_t trial_hash, trial_bytes;
  uint32_t trial_decode_us, trial_write_us, trial_hash_us, trial_clock_hz;
  uint64_t (*trial_clock)(void);""")
        else:
            modified = modified.replace('"../../../experiments/dlp_lossless/decoder.h"', '"decoder.h"')
            modified = replace(modified, "#include \"decoder.h\"", """#include "decoder.h"
extern uint32_t solar_os_dlp_trial_framebuffer_hash(void);""")
            modified = replace(modified, """    a->length = 12;
    return SDLP_OK;""", """    sdlp_put_u32(a->payload + 12, d->trial_decode_us);
    sdlp_put_u32(a->payload + 16, d->trial_write_us);
    sdlp_put_u32(a->payload + 20, d->trial_hash_us);
    sdlp_put_u32(a->payload + 24, d->trial_clock_hz);
    a->length = 28;
    return SDLP_OK;
  }
  if (q->opcode == 9) {
    if (q->length) return SDLP_BAD_ARGUMENT;
    sdlp_put_u32(a->payload, solar_os_dlp_trial_framebuffer_hash());
    a->length = 4;
    return SDLP_OK;""")
            modified = replace(modified, "    d->trial_bytes = 0;", """    d->trial_bytes = 0;
    d->trial_decode_us = d->trial_write_us = d->trial_hash_us = 0;""")
            modified = replace(modified, "      bool decoded;", """      bool decoded;
      uint64_t trial_decode_started = d->trial_clock ? d->trial_clock() : 0;""")
            modified = replace(modified, "      if (!decoded) return SDLP_BAD_ARGUMENT;", """      if (d->trial_clock)
        d->trial_decode_us += (uint32_t)(d->trial_clock() - trial_decode_started);
      if (!decoded) return SDLP_BAD_ARGUMENT;""")
            modified = replace(modified, "    r = d->backend.write(ctx, s->offset, bytes, n);", """    uint64_t trial_write_started = d->trial_clock ? d->trial_clock() : 0;
    r = d->backend.write(ctx, s->offset, bytes, n);
    if (d->trial_clock)
      d->trial_write_us += (uint32_t)(d->trial_clock() - trial_write_started);""")
            modified = replace(modified, "      for (uint32_t k = 0; k < n; k++)\n        d->trial_hash", """      uint64_t trial_hash_started = d->trial_clock ? d->trial_clock() : 0;
      for (uint32_t k = 0; k < n; k++)
        d->trial_hash""")
            modified = replace(modified, "      d->trial_bytes += n;", """      d->trial_bytes += n;
      if (d->trial_clock)
        d->trial_hash_us += (uint32_t)(d->trial_clock() - trial_hash_started);""")
        changes[name] = original, modified
    name = "src/main.c"
    original = (trial / name).read_text()
    modified = replace(original, '#include "config.h"', '#include "config.h"\n#include "hardware/clocks.h"')
    modified = replace(modified, '    panic("Invalid SDLP configuration");', """    panic("Invalid SDLP configuration");
  display_link_device(DISPLAY_LINK_USB)->trial_clock = time_us_64;
  display_link_device(DISPLAY_LINK_USB)->trial_clock_hz = clock_get_hz(clk_sys);""")
    changes[name] = original, modified
    name = "src/framebuffer.c"
    original = (trial / name).read_text()
    modified = original + '''
/* Query only after PRESENT, outside benchmark timing. Canonical RGB565 BE. */
uint32_t solar_os_dlp_trial_framebuffer_hash(void) {
  _Static_assert(SDLP_NATIVE_STORAGE == SDLP_STORAGE_RGB565, "RGB565 RAM trial");
  uint32_t hash = UINT32_C(2166136261);
  for (size_t i = 0; i < (size_t)SDLP_WIDTH * SDLP_HEIGHT; i++) {
    hash = (hash ^ (display_pixels[i] >> 8)) * UINT32_C(16777619);
    hash = (hash ^ (display_pixels[i] & 255)) * UINT32_C(16777619);
  }
  return hash;
}
'''
    changes[name] = original, modified
    name = "CMakeLists.txt"
    original = (trial / name).read_text()
    modified = original + '''
# Explicit private experiment: all consumer layouts must match the library.
target_compile_definitions(display_link PUBLIC SOLAR_OS_DLP_LOSSLESS_TRIAL=1)
'''
    changes[name] = original, modified
    patches = []
    for name, (original, modified) in changes.items():
        (trial / name).write_text(modified)
        patches.extend(difflib.unified_diff(original.splitlines(True), modified.splitlines(True),
                                          fromfile="a/" + name, tofile="b/" + name))
    shutil.copy2(HERE / "decoder.h", trial / "protocol/include/decoder.h")
    patch = "".join(patches)
    (args.output / "receiver.patch").write_text(patch)
    hashes = {str(p.relative_to(snapshot)): hashlib.sha256(p.read_bytes()).hexdigest()
              for p in snapshot.rglob("*") if p.is_file()}
    manifest = {"source_commit": subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=args.source, text=True).strip(),
        "snapshot_sha256": hashes, "patch_sha256": hashlib.sha256(patch.encode()).hexdigest()}
    (args.output / "snapshot.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Prepared {snapshot} and {trial}; source checkout unchanged")


if __name__ == "__main__":
    main()
