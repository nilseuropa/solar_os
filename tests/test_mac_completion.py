from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class MacCompletionTest(unittest.TestCase):
    def test_paths_options_and_speed_values(self):
        source = (ROOT / "src/apps/solar_os_shell.c").read_text()
        arrays = "\n".join(re.findall(
            r"^static const char \* const (?:mac_\w+|path_mac\w*)\[\] = .*?;", source, re.M))
        rules = "\n".join(re.findall(
            r"^    (?:SHELL_COMPLETION_(?:OPTIONS|PATH|STATIC)\(path_mac[^\n]+|"
            r"\{\.path = path_mac_floppy[^\n]+)", source, re.M))
        macros = []
        lines = source.splitlines()
        for name in ("SHELL_COMPLETION_OPTIONS", "SHELL_COMPLETION_PATH", "SHELL_COMPLETION_STATIC"):
            index = next(i for i, line in enumerate(lines) if line.startswith("#define " + name + "("))
            while True:
                macros.append(lines[index])
                if not lines[index].endswith("\\"):
                    break
                index += 1
        code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_shell_completion.h"
#define SHELL_ARRAY_COUNT(a) (sizeof(a)/sizeof(a[0]))
#define SHELL_COMPLETION_ANY "*"
#define SHELL_COMPLETION_FLAG_PATH 1U
#define SHELL_COMPLETION_FLAG_DIRS_ONLY 2U
typedef struct {
    const char * const *path; size_t path_count;
    const char * const *values; size_t value_count;
    const char *required_prefix; unsigned flags;
} shell_completion_rule_t;
''' + "\n".join(macros) + "\n" + arrays + "\nstatic const shell_completion_rule_t rules[] = {\n" + rules + "\n};\n"
        code += production_functions(source, (
            "shell_completion_path_matches", "shell_completion_normalize_mac_arguments"))
        code += r'''
static void check(const char *input, bool expect_path, const char *expect_values) {
    char storage[12][128] = {{0}}, values[128] = "";
    const char *tokens[12]; size_t starts[12], count; bool trailing;
    assert(solar_os_shell_completion_parse(input, strlen(input), &storage[0][0],
        sizeof(storage[0]), 12, starts, &count, &trailing));
    size_t completed = trailing ? count : count - 1;
    const char *prefix = trailing ? "" : storage[count - 1];
    for (size_t i = 0; i < completed; ++i) tokens[i] = storage[i];
    bool speed_value = shell_completion_normalize_mac_arguments(tokens, &completed);
    bool path = false;
    size_t best = (size_t)-1;
    for (size_t i = 0; i < SHELL_ARRAY_COUNT(rules); ++i) {
        const shell_completion_rule_t *rule = &rules[i];
        size_t wildcards;
        if (!shell_completion_path_matches(rule, tokens, completed, &wildcards)) continue;
        if (rule->flags & SHELL_COMPLETION_FLAG_PATH) { path = !speed_value; continue; }
        if (rule->required_prefix && strncmp(prefix, rule->required_prefix, strlen(rule->required_prefix))) continue;
        if (wildcards < best) best = wildcards;
    }
    for (size_t i = 0; i < SHELL_ARRAY_COUNT(rules); ++i) {
        const shell_completion_rule_t *rule = &rules[i];
        size_t wildcards;
        if (rule->flags & SHELL_COMPLETION_FLAG_PATH) continue;
        if (!shell_completion_path_matches(rule, tokens, completed, &wildcards) || wildcards != best) continue;
        if (rule->required_prefix && strncmp(prefix, rule->required_prefix, strlen(rule->required_prefix))) continue;
        for (size_t j = 0; j < rule->value_count; ++j) {
            assert(strcmp(rule->values[j], "--self-test"));
            if (strncmp(rule->values[j], prefix, strlen(prefix))) continue;
            if (values[0]) strcat(values, "|");
            strcat(values, rule->values[j]);
        }
    }
    if (path != expect_path || strcmp(values, expect_values)) {
        fprintf(stderr, "%s: path=%d values=%s\n", input, path, values);
        assert(false);
    }
}
int main(void) {
    check("mac ", true, "");
    check("mac --", true, "--pointer-speed");
    check("mac --self", true, "");
    check("mac /sdcard/mac/m", true, "");
    check("mac rom --", true, "--floppy|--pointer-speed");
    check("mac rom --fl", true, "--floppy");
    check("mac rom --poi", true, "--pointer-speed");
    check("mac rom --pointer-speed ", false, "1|2|5|10");
    check("mac rom --pointer-speed 1", false, "1|10");
    check("mac rom --pointer-speed 7", false, "");
    check("mac --pointer-speed 5 ", true, "");
    check("mac --pointer-speed 5 rom --", true, "--floppy|--pointer-speed");
    check("mac rom --pointer-speed 2 --floppy ", true, "");
    check("mac rom --floppy --", true, "");
    check("mac rom disk --", false, "--pointer-speed");
    check("mac rom --floppy disk --", false, "--pointer-speed");
    check("mac rom --floppy disk --pointer-speed ", false, "1|2|5|10");
    check("mac \"/sdcard/My Mac/Plus.rom\" --pointer-speed 5 --floppy \"/sdcard/My Mac/Sys", true, "");
    check("other rom --", false, "");
    puts("Mac path, option and pointer speed completion: PASS");
}
'''
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "completion"
            subprocess.run([
                "cc", "-x", "c", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "src/shell"), "-",
                str(ROOT / "src/shell/solar_os_shell_completion.c"), "-o", str(binary),
            ], input=code, text=True, check=True)
            subprocess.run([str(binary)], check=True)
