/* SolarOS MicroPython embed configuration. */

#include <stdint.h>
#include <stddef.h>

#include <port/mpconfigport_common.h>

#define MP_SSIZE_MAX                           INTPTR_MAX
#define MICROPY_CONFIG_ROM_LEVEL                (MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES)

#define MICROPY_ENABLE_COMPILER                 (1)
#define MICROPY_ENABLE_GC                       (1)
#define MICROPY_ENABLE_FINALISER                (1)
#define MICROPY_NLR_SETJMP                      (1)
#define MICROPY_PERSISTENT_CODE_LOAD            (1)
#define MICROPY_FLOAT_IMPL                      (MICROPY_FLOAT_IMPL_FLOAT)
#define MICROPY_LONGINT_IMPL                    (MICROPY_LONGINT_IMPL_LONGLONG)
#define MICROPY_GCREGS_SETJMP                  (1)
#define MICROPY_STACK_CHECK_MARGIN              (1024U)

/* SolarOS-backed files and source/bytecode imports. */
#define MICROPY_ENABLE_EXTERNAL_IMPORT          (1)
#define MICROPY_PY_BUILTINS_EXECFILE            (0)
#define MICROPY_PY_BUILTINS_INPUT               (0)
#define MICROPY_PY_IO                           (1)
#define MICROPY_PY_SYS_STDFILES                 (0)

/* Selected standard modules supported by the SolarOS embed port. */
#define MICROPY_PY_JSON                         (1)
#define MICROPY_PY_BINASCII                     (1)
#define MICROPY_PY_HASHLIB                      (1)
#define MICROPY_PY_HASHLIB_MD5                  (0)
#define MICROPY_PY_HASHLIB_SHA1                 (0)
#define MICROPY_PY_HASHLIB_SHA256               (1)
#define MICROPY_PY_RANDOM                       (1)
#define MICROPY_PY_MATH                         (1)
#define MICROPY_PY_STRUCT                       (1)
#define MICROPY_PY_COLLECTIONS                  (1)

uint32_t solar_os_micropython_random_seed(void);
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC        (solar_os_micropython_random_seed())

/* Other extmod features remain disabled until their port integration exists. */
#define MICROPY_PY_ASYNCIO                      (0)
#define MICROPY_PY_UCTYPES                      (0)
#define MICROPY_PY_DEFLATE                      (0)
#define MICROPY_PY_OS                           (0)
#define MICROPY_PY_RE                           (0)
#define MICROPY_PY_HEAPQ                        (0)
#define MICROPY_PY_SELECT                       (0)
#define MICROPY_PY_TIME                         (0)
#define MICROPY_PY_FRAMEBUF                     (0)
#define MICROPY_PY_PLATFORM                     (0)

/* Networking remains owned by typed SolarOS service bindings. */
#define MICROPY_PY_LWIP                         (0)
#define MICROPY_PY_SSL                          (0)
#define MICROPY_PY_WEBSOCKET                    (0)

#define MICROPY_PY_GC                           (1)
#define MICROPY_PY_SYS                          (1)
#define MICROPY_PY_SYS_PLATFORM                 "solaros"
#define MICROPY_PY_SYS_ARGV                     (1)
#define MICROPY_PY_SYS_EXIT                     (1)
#define MICROPY_PY_MICROPYTHON                  (1)
#define MICROPY_PY_BUILTINS_MIN_MAX             (1)
#define MICROPY_PY_BUILTINS_BYTEARRAY           (1)
#define MICROPY_PY_ARRAY                        (1)
#define MICROPY_KBD_EXCEPTION                   (1)
#define MICROPY_HELPER_REPL                     (1)

#define MICROPY_ERROR_REPORTING                 (MICROPY_ERROR_REPORTING_TERSE)
#define MICROPY_WARNINGS                        (0)
#define MICROPY_READER_POSIX                    (1)

#define SOLAR_OS_MICROPYTHON_PATH_MAX           (160)

int solar_os_micropython_resolve_path(const char *input,
                                      char *output,
                                      size_t output_len);

void solar_os_micropython_vm_hook(void);

#define MICROPY_VM_HOOK_LOOP                    solar_os_micropython_vm_hook();
