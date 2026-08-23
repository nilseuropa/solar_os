#!/usr/bin/env python3
"""Regenerate the pinned MicroPython embed package used by SolarOS."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile


REPOSITORY = Path(__file__).resolve().parents[1]
COMPONENT = REPOSITORY / "components" / "micropython_embed"
PACKAGE = COMPONENT / "micropython_embed"
QSTR_DEFS = COMPONENT / "qstrdefs.h"
PORT_OVERRIDES = COMPONENT / "overrides" / "port"
PORT_QSTR_SOURCES = (Path("solaros_file.c"),)

SELECTED_EXTMOD_SOURCES = (
    Path("extmod/modbinascii.c"),
    Path("extmod/modhashlib.c"),
    Path("extmod/modjson.c"),
    Path("extmod/modrandom.c"),
)
SELECTED_EXTMOD_SUPPORT = (
    Path("lib/crypto-algorithms/sha256.c"),
    Path("lib/crypto-algorithms/sha256.h"),
)
SOURCE_REPLACEMENTS = {
    Path("py/compile.c"): (
        (
            "#if MICROPY_ENABLE_COMPILER\n\n",
            "#if MICROPY_ENABLE_COMPILER\n\n"
            "MP_REGISTER_ROOT_POINTER(void *solar_os_active_parse_tree_chunk);\n\n",
        ),
        (
            "void mp_compile_to_raw_code(mp_parse_tree_t *parse_tree, qstr source_file, bool is_repl, mp_compiled_module_t *cm) {\n"
            "    // put compiler state on the stack, it's relatively small\n",
            "void mp_compile_to_raw_code(mp_parse_tree_t *parse_tree, qstr source_file, bool is_repl, mp_compiled_module_t *cm) {\n"
            "    // Keep the parse chunks alive even if a collection cannot find the\n"
            "    // caller's parse_tree local through the native task stack.\n"
            "    MP_STATE_VM(solar_os_active_parse_tree_chunk) = parse_tree->chunk;\n\n"
            "    // put compiler state on the stack, it's relatively small\n",
        ),
        (
            "    mp_parse_tree_clear(parse_tree);\n\n"
            "    // free the scopes\n",
            "    mp_parse_tree_clear(parse_tree);\n"
            "    MP_STATE_VM(solar_os_active_parse_tree_chunk) = NULL;\n\n"
            "    // free the scopes\n",
        ),
    ),
    Path("py/reader.c"): (
        (
            "int fd = open(qstr_str(filename), O_RDONLY, 0644);\n",
            "char resolved[SOLAR_OS_MICROPYTHON_PATH_MAX];\n"
            "    if (solar_os_micropython_resolve_path(qstr_str(filename), resolved, sizeof(resolved)) != 0) {\n"
            "        mp_raise_OSError_with_filename(errno, qstr_str(filename));\n"
            "    }\n"
            "    int fd = open(resolved, O_RDONLY, 0644);\n",
        ),
    ),
}

MICROPYTHON_URL = "https://github.com/micropython/micropython.git"
MICROPYTHON_COMMIT = "d901e9834939372f68974010f32e146596a69bb0"
MICROPYTHON_GIT_TAG = "d901e98349"
MICROPYTHON_GIT_HASH = "d901e98"
MICROPYTHON_SOURCE_DATE_EPOCH = "1781248016"

REQUIRED_OUTPUT = {
    Path("genhdr/moduledefs.h"): (
        "MODULE_DEF_ARRAY",
        "MODULE_DEF_BINASCII",
        "MODULE_DEF_COLLECTIONS",
        "MODULE_DEF_HASHLIB",
        "MODULE_DEF_IO",
        "MODULE_DEF_JSON",
        "MODULE_DEF_MATH",
        "MODULE_DEF_RANDOM",
        "MODULE_DEF_STRUCT",
    ),
    Path("genhdr/qstrdefs.generated.h"): (
        "MP_QSTR_StringIO",
        "MP_QSTR_FileIO",
        "MP_QSTR_TextIOWrapper",
        "MP_QSTR_dsp_processor",
        "MP_QSTR_execute",
        "MP_QSTR_process",
        "MP_QSTR_reset",
    ),
}


def run(
    command: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
) -> None:
    subprocess.run(command, cwd=cwd, env=env, check=True)


def prepare_upstream(repository: Path | None, temporary: Path) -> Path:
    if repository is None:
        repository = temporary / "micropython-repository"
        run(["git", "init", "--quiet", str(repository)])
        run(
            ["git", "-C", str(repository), "remote", "add", "origin", MICROPYTHON_URL]
        )
        run([
            "git",
            "-C",
            str(repository),
            "fetch",
            "--quiet",
            "--depth=1",
            "origin",
            MICROPYTHON_COMMIT,
        ])
    else:
        repository = repository.resolve()
        if not repository.is_dir():
            raise ValueError(f"MicroPython repository does not exist: {repository}")

    result = subprocess.run(
        [
            "git",
            "-C",
            str(repository),
            "cat-file",
            "-e",
            f"{MICROPYTHON_COMMIT}^{{commit}}",
        ],
        check=False,
    )
    if result.returncode != 0:
        raise ValueError(
            f"MicroPython repository does not contain pinned commit {MICROPYTHON_COMMIT}"
        )

    archive_path = temporary / "micropython.tar"
    source = temporary / "micropython-source"
    source.mkdir()
    run([
        "git",
        "-C",
        str(repository),
        "archive",
        "--format=tar",
        f"--output={archive_path}",
        MICROPYTHON_COMMIT,
    ])
    with tarfile.open(archive_path, "r") as archive:
        archive.extractall(source, filter="data")
    return source


def generate(source: Path, temporary: Path) -> Path:
    build = temporary / "build"
    package = temporary / "micropython_embed"
    for relative, replacements in SOURCE_REPLACEMENTS.items():
        path = source / relative
        text = path.read_text(encoding="utf-8")
        for original, replacement in replacements:
            if text.count(original) != 1:
                raise RuntimeError(f"unexpected upstream content in {relative}")
            text = text.replace(original, replacement)
        path.write_text(text, encoding="utf-8")

    upstream_port = source / "ports" / "embed" / "port"
    for override in sorted(PORT_OVERRIDES.iterdir()):
        if override.is_file():
            shutil.copyfile(override, upstream_port / override.name)

    makefile = temporary / "micropython-embed-selected.mk"
    makefile.write_text(
        "SRC_QSTR := "
        + " ".join(
            str(path)
            for path in SELECTED_EXTMOD_SOURCES
            + tuple(
                Path("ports/embed/port") / relative
                for relative in PORT_QSTR_SOURCES
            )
        )
        + "\ninclude "
        + str(source / "ports" / "embed" / "embed.mk")
        + "\n",
        encoding="utf-8",
    )
    environment = os.environ.copy()
    environment.update({
        "LC_ALL": "C",
        "MICROPY_GIT_HASH": MICROPYTHON_GIT_HASH,
        "MICROPY_GIT_TAG": MICROPYTHON_GIT_TAG,
        "PYTHONHASHSEED": "0",
        "SOURCE_DATE_EPOCH": MICROPYTHON_SOURCE_DATE_EPOCH,
    })
    run([
        "make",
        "-f",
        str(makefile),
        f"MICROPYTHON_TOP={source}",
        f"BUILD={build}",
        f"PACKAGE_DIR={package}",
        f"QSTR_DEFS={QSTR_DEFS}",
    ], cwd=COMPONENT, env=environment)

    for relative in SELECTED_EXTMOD_SOURCES + SELECTED_EXTMOD_SUPPORT:
        destination = package / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / relative, destination)

    for override in sorted(PORT_OVERRIDES.iterdir()):
        if override.is_file():
            shutil.copyfile(override, package / "port" / override.name)

    for relative, markers in REQUIRED_OUTPUT.items():
        output = package / relative
        text = output.read_text(encoding="utf-8") if output.is_file() else ""
        for marker in markers:
            if marker not in text:
                raise RuntimeError(f"generated {relative} does not contain {marker}")
    return package


def files(root: Path) -> dict[Path, bytes]:
    if not root.exists():
        return {}
    return {
        path.relative_to(root): path.read_bytes()
        for path in sorted(root.rglob("*"))
        if path.is_file()
    }


def differences(expected: Path, actual: Path) -> list[str]:
    expected_files = files(expected)
    actual_files = files(actual)
    result = [f"missing {path}" for path in sorted(expected_files.keys() - actual_files.keys())]
    result.extend(
        f"unexpected {path}" for path in sorted(actual_files.keys() - expected_files.keys())
    )
    result.extend(
        f"changed {path}"
        for path in sorted(expected_files.keys() & actual_files.keys())
        if expected_files[path] != actual_files[path]
    )
    return result


def update(expected: Path, actual: Path) -> list[str]:
    changes = differences(expected, actual)
    expected_files = files(expected)
    actual_files = files(actual)
    for relative in sorted(actual_files.keys() - expected_files.keys(), reverse=True):
        (actual / relative).unlink()
    for relative, content in expected_files.items():
        destination = actual / relative
        if actual_files.get(relative) == content:
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(expected / relative, destination)
    for directory in sorted(
        (path for path in actual.rglob("*") if path.is_dir()),
        key=lambda path: len(path.parts),
        reverse=True,
    ):
        if not any(directory.iterdir()):
            directory.rmdir()
    return changes


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--micropython-repo",
        type=Path,
        help="existing MicroPython Git repository containing the pinned commit",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="verify the vendored package without updating it",
    )
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="solaros-micropython-") as directory:
        temporary = Path(directory)
        source = prepare_upstream(args.micropython_repo, temporary)
        generated = generate(source, temporary)
        changes = differences(generated, PACKAGE) if args.check else update(generated, PACKAGE)

    if changes:
        for change in changes:
            print(change)
        if args.check:
            print("MicroPython embed package is not up to date")
            return 1
        print(f"Updated MicroPython embed package ({len(changes)} changes)")
    else:
        print("MicroPython embed package is up to date")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
