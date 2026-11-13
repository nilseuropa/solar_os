"""Generate the small ABI adaptations for pinned libgit2 1.9.7 on Xtensa."""
from pathlib import Path
import sys
import re
sys.dont_write_bytecode = True
from state_sources import INITIALIZED_STATE, move_initializers

source, output = (Path(p).resolve() for p in sys.argv[1:])
patches = {
    # The SHA1 collision-detection vectors are immutable lookup data. Without
    # const, ESP-IDF copies the 11,352-byte table into internal SRAM at boot.
    "src/util/hash/sha1dc/ubc_check.h": [
        ("extern dv_info_t sha1_dvs[];", "extern const dv_info_t sha1_dvs[];", 1)],
    "src/util/hash/sha1dc/ubc_check.c": [
        ("dv_info_t sha1_dvs[] =", "const dv_info_t sha1_dvs[] =", 1)],
    "src/util/hash/sha1dc/sha1.c": [],
    "src/libgit2/libgit2.c": [
        ("int git_libgit2_init(void)\n{", "int git_libgit2_init(void)\n{\n\tsolar_os_git_state_init();", 1)],
    "src/libgit2/settings.c": [
        ("int git_libgit2_opts(int key, ...)\n{", "int git_libgit2_opts(int key, ...)\n{\n\tsolar_os_git_state_init();", 1)],
    # Streaming buffers must fit an embedded foreground stack. Upstream's
    # 64 KiB arrays reserve their space even when an optional copy is skipped.
    "src/util/filebuf.c": [("buffer[GIT_BUFSIZE_FILEIO]", "buffer[2048]", 1)],
    "src/util/futils.c": [("buffer[GIT_BUFSIZE_FILEIO]", "buffer[2048]", 1)],
    "src/libgit2/blob.c": [("buffer[GIT_BUFSIZE_FILEIO]", "buffer[2048]", 1)],
    "src/libgit2/odb.c": [("buffer[GIT_BUFSIZE_FILEIO]", "buffer[2048]", 1)],
    "src/libgit2/filter.c": [("buf[GIT_BUFSIZE_FILTERIO]", "buf[2048]", 1)],
    "src/util/thread.c": [
        ("void *value;\n\tvoid (GIT_SYSTEM_CALL *destroy_fn)",
         "bool used;\n\tvoid *value;\n\tvoid (GIT_SYSTEM_CALL *destroy_fn)", 1),
        ("static int tlsdata_cnt = 0;", "", 1),
        ("\tif (tlsdata_cnt >= TLSDATA_MAX)\n\t\treturn -1;\n\n"
         "\ttlsdata_values[tlsdata_cnt].value = NULL;\n"
         "\ttlsdata_values[tlsdata_cnt].destroy_fn = destroy_fn;\n\n"
         "\t*key = tlsdata_cnt;\n\ttlsdata_cnt++;",
         "\tint slot;\n\tfor (slot = 0; slot < TLSDATA_MAX; slot++)\n"
         "\t\tif (!tlsdata_values[slot].used) break;\n"
         "\tif (slot == TLSDATA_MAX) return -1;\n"
         "\ttlsdata_values[slot].used = true;\n"
         "\ttlsdata_values[slot].value = NULL;\n"
         "\ttlsdata_values[slot].destroy_fn = destroy_fn;\n\t*key = slot;", 1),
        ("key < 0 || key > tlsdata_cnt",
         "key < 0 || key >= TLSDATA_MAX || !tlsdata_values[key].used", 3),
        ("tlsdata_values[key].destroy_fn = NULL;",
         "tlsdata_values[key].destroy_fn = NULL;\n\ttlsdata_values[key].used = false;", 1),
    ],
    "src/libgit2/checkout.c": [("unsigned int *actions,", "uint32_t *actions,", 3)],
    "src/libgit2/config.c": [
        ("git_config_parse_int32(out, value)", "solar_os_git_parse_int(out, value)", 2)],
    "src/libgit2/repository.c": [("git_config_get_int32(version,", "solar_os_git_config_int(version,", 1)],
    "src/libgit2/revparse.c": [("git__strntol32(&parsed,", "solar_os_git_strntoint(&parsed,", 1)],
    "src/libgit2/signature.c": [("git__strntol32(&offset,", "solar_os_git_strntoint(&offset,", 1)],
    "src/libgit2/tree-cache.c": [("git__strntol32(&count,", "solar_os_git_strntoint(&count,", 2)],
    "src/libgit2/transports/local.c": [
        ("local_counting(int stage, unsigned int current, unsigned int total,", "local_counting(int stage, uint32_t current, uint32_t total,", 1)],
    "deps/pcre2/pcre2_compile.c": [
        ("int32_t min = 0;", "int min = 0;", 1),
        ("int32_t max = REPEAT_UNLIMITED;", "int max = REPEAT_UNLIMITED;", 1),
        ("uint32_t c16workspace[C16_WORK_SIZE];",
         "uint32_t *c16workspace __attribute__((cleanup(solar_os_git_workspace_release))) = NULL;", 1),
        ("*erroroffset = 0;\n\n/* There must be a pattern",
         "*erroroffset = 0;\n"
         "c16workspace = solar_os_git_workspace_alloc(sizeof(uint32_t) * C16_WORK_SIZE);\n"
         "if (c16workspace == NULL) { *errorptr = ERR21; return NULL; }\n"
         "cworkspace = (PCRE2_UCHAR *)c16workspace;\n\n/* There must be a pattern", 1),
    ],
}
for name in INITIALIZED_STATE:
    patches.setdefault(name, [])
for name, replacements in patches.items():
    text = (source / name).read_text()
    for before, after, count in replacements:
        if text.count(before) != count:
            raise SystemExit(f"libgit2 patch mismatch: {name}: {before}")
        text = text.replace(before, after)
    if name in INITIALIZED_STATE:
        text = move_initializers(text, INITIALIZED_STATE[name], name)
    # Preserve quoted include lookup from the original source directory. In
    # particular, libgit2's internal config.h differs from PCRE2's generated one.
    # Consumers of a patched header must use its generated copy as well.
    parent = (source / name).parent
    def include_path(match):
        original = (parent / match[2]).resolve()
        if not original.is_file():
            return match[0]
        relative = original.relative_to(source).as_posix()
        target = output / original.name if relative in patches else original
        return f'{match[1]}"{target}"'
    text = re.sub(r'(#\s*include\s*)"([^"]+)"',
        include_path, text)
    target = output / Path(name).name
    if not target.exists() or target.read_text() != text:
        target.write_text(text)
print(";".join(name for name in patches if name.endswith(".c")))
