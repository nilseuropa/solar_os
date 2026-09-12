#!/usr/bin/env python3
"""Generate build-local NimBLE overlays; never modify the installed SDK."""
import argparse
import hashlib
from pathlib import Path

HOST = Path("components/bt/host/nimble/nimble/nimble/host/src")
HASHES = {
    "ble_gatts.c": "6544a5839bb51a584dc563f01cfea220b9203790568c182a4cfe95ed89046096",
    "ble_att_svr.c": "6a0c33d7c11a81fd022c13349a3da8b8da82993c41177568a8a5238cbf2c0799",
}
PATCH_DIR = Path(__file__).resolve().parents[1] / "patches/nimble"


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise ValueError("NimBLE patch anchor mismatch; review SDK before building")
    return source.replace(old, new, 1)


def transform(name, original):
    if hashlib.sha256(original).hexdigest() != HASHES[name]:
        raise ValueError(f"Unsupported NimBLE SDK source: {name}; expected ESP-IDF 5.5.5. "
                         "Review and retest the overlay; do not bypass the hash guard.")
    source = original.decode("utf-8")
    if name == "ble_gatts.c":
        start = source.index("static struct ble_gatts_clt_cfg * ble_gatts_get_last_cfg(")
        end = source.index("\nstatic int\nble_gatts_deregister_svc", start)
        source = source[:start] + (PATCH_DIR / "dynamic_services.inc").read_text() + source[end:]
        source = replace_once(source,
            "    return ble_gatts_num_cfgable_chrs == 0 ||\n"
            "           ble_gatts_clt_cfg_pool.mp_num_free > 0;",
            "#if MYNEWT_VAL(BLE_DYNAMIC_SERVICE) && !MYNEWT_VAL(MP_RUNTIME_ALLOC)\n"
            "    /* Dynamic connection init can fall back to heap; it handles OOM. */\n"
            "    return 1;\n#else\n"
            "    return ble_gatts_num_cfgable_chrs == 0 ||\n"
            "           ble_gatts_clt_cfg_pool.mp_num_free > 0;\n#endif")
        source = replace_once(source,
            "            ble_gatts_remove_clt_cfg(&ble_gatts_clt_cfgs, chr_val_handle);",
            "            if (ble_gatts_remove_clt_cfg(&ble_gatts_clt_cfgs, chr_val_handle) == 0) {\n"
            "                ble_gatts_num_cfgable_chrs--;\n            }")
    else:
        source = replace_once(source,
            "    entry = ble_att_svr_entry_alloc();\n",
            "    /* Exhaustion is an error, not an assertion or handle wraparound. */\n"
            "    if (ble_att_svr_id == UINT16_MAX) {\n"
            "        return BLE_HS_ENOMEM;\n    }\n"
            "    entry = ble_att_svr_entry_alloc();\n")
        source = replace_once(source, "\nuint16_t\nble_att_svr_prev_handle(void)",
            "\n" + (PATCH_DIR / "att_rollback.inc").read_text() +
            "\nuint16_t\nble_att_svr_prev_handle(void)")
    return source


def generate(idf, output):
    idf, output = idf.resolve(), output.resolve()
    if output.is_relative_to(idf):
        raise ValueError("NimBLE overlay output must not be inside the installed SDK")
    if any((output / name).is_symlink() for name in HASHES):
        raise ValueError("NimBLE overlay output must not contain source-file symlinks")
    # Validate all inputs before updating any generated file.
    rendered = {name: transform(name, (idf / HOST / name).read_bytes()) for name in HASHES}
    output.mkdir(parents=True, exist_ok=True)
    for name, content in rendered.items():
        path = output / name
        if not path.exists() or path.read_text() != content:
            path.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--idf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        generate(args.idf.resolve(), args.output.resolve())
    except (ValueError, OSError) as exc:
        parser.exit(1, f"NimBLE overlay: {exc}\n")
