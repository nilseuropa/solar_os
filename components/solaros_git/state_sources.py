"""Initialized libgit2 globals audited for ordinary Git task access only."""
import re

# Non-zero initializers cannot be put directly in ESP-IDF external BSS. Keep
# typed, read-only defaults in flash and register their external destinations.
# Zero-initialized globals are handled by the component linker fragment.
INITIALIZED_STATE = {
    "src/libgit2/iterator.c": ["callbacks"],
    "src/libgit2/libgit2.c": ["init_fns"],
    "src/libgit2/merge_driver.c": ["git_merge_driver__binary", "git_merge_driver__union", "git_merge_driver__text"],
    "src/libgit2/mwindow.c": ["git_mwindow__mapped_limit", "git_mwindow__window_size"],
    "src/libgit2/object.c": ["git_object__strict_input_validation"],
    "src/libgit2/refs.c": ["git_reference__enable_symbolic_ref_target_validation"],
    "src/libgit2/submodule.c": ["_sm_recurse_map", "_sm_ignore_map", "_sm_update_map"],
    "src/libgit2/sysdir.c": ["git_sysdir__dirs"],
    "src/libgit2/transport.c": ["transports", "local_transport_definition", "git_subtransport_definition", "http_subtransport_definition"],
    "src/libgit2/transports/httpclient.c": ["auth_schemes"],
    "src/libgit2/transports/smart_protocol.c": ["git_smart__ofs_delta_enabled"],
    "src/util/alloc.c": ["git__allocator"],
    "src/util/errors.c": ["no_error", "tlsdata_error", "uninitialized_error", "oom_error"],
    "src/libgit2/odb.c": ["git_odb__strict_hash_verification", "git_odb__packed_priority", "git_odb__loose_priority"],
    "src/libgit2/repo_template.h": ["repo_template"],
    "src/libgit2/repository.c": ["user_extensions", "state_files", "git_repository__reserved_names_posix_len", "git_repository__reserved_names_posix", "git_repository__reserved_names_win32_len", "git_repository__reserved_names_win32", "git_repository__validate_ownership"],
    "src/libgit2/attr.c": ["git_attr__unset", "git_attr__false", "git_attr__true"],
    "src/libgit2/cache.c": ["git_cache__max_object_size", "git_cache__max_storage", "git_cache__enabled"],
    "src/libgit2/config_cache.c": ["_configmap_abbrev", "_configmap_logallrefupdates", "_configmap_safecrlf", "_configmap_autocrlf", "_configmap_eol"],
    "src/libgit2/config_parse.c": ["git_config_escaped", "git_config_escapes"],
    "src/libgit2/diff_driver.c": ["diff_driver_text", "diff_driver_binary"],
    "src/libgit2/userdiff.h": ["builtin_defs"],
    "src/libgit2/indexer.c": ["git_indexer__max_object_size", "git_indexer__max_objects"],
    "src/libgit2/transports/auth.c": ["basic_context"],
    "deps/pcre2/pcre2_context.c": ["PRIV(default_match_context)", "PRIV(default_compile_context)", "PRIV(default_convert_context)"],
}

# Skip punctuation inside C strings/comments while locating the initializer's
# terminating semicolon. No general source rewriting is attempted: only the
# named declarations from the pinned version above are accepted.
TOKENS = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/|[{}\[\]();]', re.S)


def move_initializers(text, names, filename):
    for name in names:
        pattern = re.compile(
            r'(?m)^(?P<indent>[ \t]*)(?P<type>[A-Za-z_][\w \t*]*[ \t*])'
            + re.escape(name) + r'(?P<array>[ \t]*\[[^\n;=]*\])?[ \t]*=[ \t]*')
        matches = list(pattern.finditer(text))
        if not matches:
            raise SystemExit(f"Git state declaration missing: {filename}: {name}")
        for index, match in reversed(list(enumerate(matches))):
            depth = 0
            for token in TOKENS.finditer(text, match.end()):
                value = token.group()
                if value in ("{", "[", "("):
                    depth += 1
                elif value in ("}", "]", ")"):
                    depth -= 1
                elif value == ";" and depth == 0:
                    end = token.start()
                    break
            else:
                raise SystemExit(f"Git state initializer missing: {filename}: {name}")
            indent = match["indent"]
            ctype = match["type"].strip()
            storage = "static " if ctype.startswith("static ") else ""
            if storage:
                ctype = ctype[len(storage):]
            label = re.sub(r'\W', '_', name)
            seed = f"solar_git_initial_{label}_{index}"
            array = match["array"] or ""
            bound = array
            if array and not array.strip()[1:-1].strip():
                bound = f"[sizeof({seed}) / sizeof({seed}[0])]"
            initializer = text[match.end():end].strip()
            replacement = (
                f"{indent}static {ctype} const {seed}{array} = {initializer};\n"
                f"{indent}{storage}{ctype} {name}{bound} SOLAR_OS_GIT_STATE_BSS;\n"
                f"{indent}SOLAR_OS_GIT_STATE_INITIALIZER({name}, {seed});")
            text = text[:match.start()] + replacement + text[end + 1:]
    return text
