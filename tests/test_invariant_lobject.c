#include <check.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>

/* We'll test luaO_pushfstring which uses vsnprintf internally.
 * This function is in lobject.c and handles string formatting.
 * We'll test that it never writes beyond its internal buffer.
 */

static void run_test_case(const char *format, const char *arg) {
    /* Fork to isolate any potential buffer overflow crash */
    pid_t pid = fork();
    if (pid == 0) {
        /* Child process: execute the actual production function */
        extern void luaO_pushfstring(const char *fmt, ...);
        
        /* This is a simplified test - in reality we'd need to set up
         * Lua state properly. Instead we'll compile a small program
         * that exercises the function directly.
         */
        char test_program[1024];
        snprintf(test_program, sizeof(test_program),
            "#include \"components/lua/lua/src/lobject.c\"\n"
            "#include \"components/lua/lua/src/lstate.c\"\n"
            "#include <stdio.h>\n"
            "#include <string.h>\n"
            "\n"
            "int main(void) {\n"
            "    lua_State *L = lua_newstate(NULL, NULL);\n"
            "    if (!L) return 1;\n"
            "    \n"
            "    /* Test the vulnerable pattern */\n"
            "    const char *result = luaO_pushfstring(L, \"%%s\", \"%s\");\n"
            "    \n"
            "    /* If we get here, no crash occurred */\n"
            "    lua_close(L);\n"
            "    return 0;\n"
            "}\n",
            arg);
        
        /* Write test program to file */
        FILE *f = fopen("test_lua.c", "w");
        if (!f) _exit(1);
        fwrite(test_program, 1, strlen(test_program), f);
        fclose(f);
        
        /* Compile and run */
        int compile_status = system("gcc -Icomponents/lua/lua/src test_lua.c -o test_lua 2>/dev/null");
        if (compile_status != 0) _exit(1);
        
        int run_status = system("./test_lua");
        _exit(run_status != 0);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        ck_assert_msg(WIFEXITED(status) && WEXITSTATUS(status) == 0,
                     "Buffer overflow or crash detected for input: %s", arg);
    } else {
        ck_abort_msg("fork failed");
    }
}

START_TEST(test_buffer_reads_never_exceed_declared_length)
{
    /* Invariant: String operations never read/write beyond allocated buffer bounds */
    const char *payloads[] = {
        "normal",                    /* Valid input */
        "A",                         /* Boundary: single char */
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", /* 256 chars */
        "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s", /* Format string attack */
        NULL
    };
    
    for (int i = 0; payloads[i] != NULL; i++) {
        run_test_case("%s", payloads[i]);
    }
}
END_TEST

Suite *security_suite(void)
{
    Suite *s;
    TCase *tc_core;

    s = suite_create("Security");
    tc_core = tcase_create("Core");

    tcase_add_test(tc_core, test_buffer_reads_never_exceed_declared_length);
    suite_add_tcase(s, tc_core);

    return s;
}

int main(void)
{
    int number_failed;
    Suite *s;
    SRunner *sr;

    s = security_suite();
    sr = srunner_create(s);

    srunner_run_all(sr, CK_NORMAL);
    number_failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    /* Cleanup */
    unlink("test_lua.c");
    unlink("test_lua");

    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}