#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "solar_os_shell_startup_policy.h"

static void test_names_and_parser(void)
{
    solar_os_shell_startup_source_t source = SOLAR_OS_SHELL_STARTUP_FLASH;

    assert(strcmp(solar_os_shell_startup_source_name(SOLAR_OS_SHELL_STARTUP_AUTO),
                  "auto") == 0);
    assert(strcmp(solar_os_shell_startup_source_name(SOLAR_OS_SHELL_STARTUP_FLASH),
                  "flash") == 0);
    assert(strcmp(solar_os_shell_startup_source_name(SOLAR_OS_SHELL_STARTUP_SD),
                  "sd") == 0);
    assert(strcmp(solar_os_shell_startup_source_name((solar_os_shell_startup_source_t)99),
                  "unknown") == 0);

    assert(solar_os_shell_parse_startup_source("auto", &source));
    assert(source == SOLAR_OS_SHELL_STARTUP_AUTO);
    assert(solar_os_shell_parse_startup_source("flash", &source));
    assert(source == SOLAR_OS_SHELL_STARTUP_FLASH);
    assert(solar_os_shell_parse_startup_source("sd", &source));
    assert(source == SOLAR_OS_SHELL_STARTUP_SD);
    assert(!solar_os_shell_parse_startup_source("default", &source));
    assert(!solar_os_shell_parse_startup_source(NULL, &source));
    assert(!solar_os_shell_parse_startup_source("auto", NULL));
}

static void test_auto_resolution(void)
{
    assert(SOLAR_OS_SHELL_STARTUP_DEFAULT == SOLAR_OS_SHELL_STARTUP_AUTO);
    assert(solar_os_shell_resolve_startup_source(
               SOLAR_OS_SHELL_STARTUP_AUTO, true, true) ==
           SOLAR_OS_SHELL_STARTUP_SD);
    assert(solar_os_shell_resolve_startup_source(
               SOLAR_OS_SHELL_STARTUP_AUTO, true, false) ==
           SOLAR_OS_SHELL_STARTUP_FLASH);
    assert(solar_os_shell_resolve_startup_source(
               SOLAR_OS_SHELL_STARTUP_AUTO, false, true) ==
           SOLAR_OS_SHELL_STARTUP_FLASH);
    assert(solar_os_shell_resolve_startup_source(
               SOLAR_OS_SHELL_STARTUP_AUTO, false, false) ==
           SOLAR_OS_SHELL_STARTUP_FLASH);
}

static void test_explicit_sources_do_not_change(void)
{
    assert(solar_os_shell_resolve_startup_source(
               SOLAR_OS_SHELL_STARTUP_FLASH, true, true) ==
           SOLAR_OS_SHELL_STARTUP_FLASH);
    assert(solar_os_shell_resolve_startup_source(
               SOLAR_OS_SHELL_STARTUP_SD, true, false) ==
           SOLAR_OS_SHELL_STARTUP_SD);
}

int main(void)
{
    test_names_and_parser();
    test_auto_resolution();
    test_explicit_sources_do_not_change();
    return 0;
}
