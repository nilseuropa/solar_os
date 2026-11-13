set(git_src "$ENV{SOLAR_OS_LIBGIT2_SOURCE_DIR}")
set(git_gen "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${git_gen}")
# The Git service serializes library calls. No child processes, mmap, or
# background pack threads are available on ESP-IDF.
set(GIT_ARCH_32 1)
set(GIT_REGEX_BUILTIN 1)
set(GIT_SHA1_COLLISIONDETECT 1)
set(GIT_SHA256_MBEDTLS 1)
set(GIT_COMPRESSION_ZLIB 1)
set(GIT_HTTPPARSER_BUILTIN 1)
set(GIT_IO_POLL 1)
set(GIT_RAND_GETENTROPY 1)
configure_file("${git_src}/src/util/git2_features.h.in" "${git_gen}/git2_features.h")
configure_file("${git_src}/src/libgit2/experimental.h.in" "${git_gen}/git2/experimental.h")

set(SUPPORT_PCRE2_8 1)
set(NEWLINE_DEFAULT 2)
set(PCRE2_LINK_SIZE 2)
set(PCRE2_MAX_VARLOOKBEHIND 255)
set(PCRE2_PARENS_NEST_LIMIT 250)
set(PCRE2_HEAP_LIMIT 1024)
set(PCRE2_MATCH_LIMIT 1000000)
set(PCRE2_MATCH_LIMIT_DEPTH 1000)
set(HAVE_ASSERT_H 1)
set(HAVE_SYS_TYPES_H 1)
set(HAVE_SYS_STAT_H 1)
set(HAVE_UNISTD_H 1)
configure_file("${git_src}/deps/pcre2/config.h.in" "${git_gen}/config.h" @ONLY)

file(GLOB git_sources "${git_src}/src/libgit2/*.c"
    "${git_src}/src/libgit2/streams/*.c" "${git_src}/src/libgit2/transports/*.c"
    "${git_src}/src/util/*.c" "${git_src}/src/util/allocators/*.c"
    "${git_src}/src/util/unix/*.c" "${git_src}/deps/llhttp/*.c"
    "${git_src}/deps/pcre2/*.c" "${git_src}/deps/zlib/*.c" "${git_src}/deps/xdiff/*.c")
list(FILTER git_sources EXCLUDE REGEX "/unix/(process|realpath)\\.c$")
# Git uses pcre2_match, never the separate DFA engine with its large stack.
list(FILTER git_sources EXCLUDE REGEX "/pcre2_(fuzzsupport|dfa_match)\\.c$")
# Keep upstream sources immutable; generated copies fix int32_t/long versus
# int pointer assumptions on Xtensa without weakening compiler diagnostics.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/patch_sources.py" "${CMAKE_CURRENT_LIST_DIR}/state_sources.py"
    "${CMAKE_CURRENT_LIST_DIR}/solar_os_git_port.h" "${CMAKE_CURRENT_LIST_DIR}/include/solar_os_git_state.h")
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_LIST_DIR}/patch_sources.py" "${git_src}" "${git_gen}"
    OUTPUT_VARIABLE git_patched_sources OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
foreach(patched IN LISTS git_patched_sources)
    get_filename_component(name "${patched}" NAME)
    list(REMOVE_ITEM git_sources "${git_src}/${patched}")
    list(APPEND git_sources "${git_gen}/${name}")
endforeach()
list(APPEND git_sources "${git_src}/src/util/hash/collisiondetect.c"
    "${git_src}/src/util/hash/mbedtls.c")
idf_component_register(SRCS ${git_sources} solar_os_git_port.c solar_os_git_state.c
    INCLUDE_DIRS "${git_src}/include" "${git_gen}" include
    PRIV_INCLUDE_DIRS . "${git_src}/src/libgit2" "${git_src}/src/libgit2/transports" "${git_src}/src/util"
        "${git_src}/deps/llhttp" "${git_src}/deps/pcre2" "${git_src}/deps/zlib" "${git_src}/deps/xdiff"
    PRIV_REQUIRES mbedtls esp_hw_support lwip
    LDFRAGMENTS git_memory.lf)
target_compile_options(${COMPONENT_LIB} PRIVATE
    "-include${CMAKE_CURRENT_LIST_DIR}/solar_os_git_port.h"
    -Wno-error -Wno-unused-function -Wno-unused-variable
    -Werror=frame-larger-than=16384)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    NO_MMAP NO_READDIR_R GIT_DISABLE_DEPRECATE_WARNINGS
    HAVE_CONFIG_H PCRE2_CODE_UNIT_WIDTH=8 PCRE2_STATIC PCRE2_EXPORT=
    PCRE2_EXP_DECL= PCRE2_EXP_DEFN= NO_VIZ STDC NO_GZIP
    HAVE_SYS_TYPES_H HAVE_STDINT_H HAVE_STDDEF_H Z_HAVE_UNISTD_H
    SHA1DC_NO_STANDARD_INCLUDES=1 "SHA1DC_CUSTOM_INCLUDE_SHA1_C=\"git2_util.h\""
    "SHA1DC_CUSTOM_INCLUDE_UBC_CHECK_C=\"git2_util.h\"")
# PlatformIO's source scanner does not track a forced include as a dependency.
# Changing the port header must invalidate every library object on that path.
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/solar_os_git_port.h" git_port_revision)
file(SHA256 "${CMAKE_CURRENT_LIST_DIR}/include/solar_os_git_state.h" git_state_revision)
string(SHA256 git_port_revision "${git_port_revision}${git_state_revision}")
target_compile_definitions(${COMPONENT_LIB} PRIVATE "SOLAR_OS_GIT_PORT_REVISION=${git_port_revision}")
