#!/usr/bin/env python3
"""Add address-aware opens to pinned VCP drivers without modifying managed sources."""
import hashlib
from pathlib import Path
import sys

HASHES = {
    "ch34x": "dd474652b8584d7ef82aa6354bd10d74860b97079408d33a3df4ffaa65b669d8",
    "cp210x": "53bbfecf143e8e76fb32df83546a6c963c87631189ac50d203f628b57ca0ec4a",
}


def patched_source(kind, source):
    if hashlib.sha256(source.encode()).hexdigest() != HASHES[kind]:
        raise ValueError("USB VCP source changed; review address-aware initialization")
    start = source.index("    // Set custom function for this driver")
    init = source[start:source.index("    return ret;", start)]
    return source + f"""

esp_err_t solar_os_usb_{kind}_open(const cdc_acm_host_open_config_t *config,
    cdc_acm_dev_hdl_t *cdc_hdl_ret)
{{
    esp_err_t ret = cdc_acm_host_open(config, cdc_hdl_ret);
{init}    return ret;
}}
"""


if __name__ == "__main__":
    kind, source, output = sys.argv[1:]
    target = Path(output)
    contents = patched_source(kind, Path(source).read_text())
    target.parent.mkdir(parents=True, exist_ok=True)
    if not target.exists() or target.read_text() != contents:
        target.write_text(contents)
