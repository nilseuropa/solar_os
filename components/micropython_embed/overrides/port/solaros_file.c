/* SolarOS-backed MicroPython file objects and external-import stat hook. */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "py/builtin.h"
#include "py/mperrno.h"
#include "py/mpthread.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/stream.h"

#define SOLAR_OS_FILE_IO_CHUNK (4096U)

typedef struct {
    mp_obj_base_t base;
    int fd;
} solar_os_file_obj_t;

extern const mp_obj_type_t solar_os_type_fileio;
extern const mp_obj_type_t solar_os_type_textio;

__attribute__((weak)) int solar_os_micropython_resolve_path(const char *input,
                                                             char *output,
                                                             size_t output_len) {
    (void)input;
    (void)output;
    (void)output_len;
    errno = ENOSYS;
    return -1;
}

extern bool solar_os_micropython_stop_requested(void);

static void solar_os_file_check_cancel(void) {
    if (solar_os_micropython_stop_requested()) {
        solar_os_micropython_check_interrupt();
    }
}

static void solar_os_file_check_open(const solar_os_file_obj_t *self) {
    if (self->fd < 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("I/O operation on closed file"));
    }
}

static const char *solar_os_file_string(mp_obj_t object, size_t *len) {
    if (!mp_obj_is_str_or_bytes(object)) {
        mp_raise_TypeError(MP_ERROR_TEXT("file must be a string or bytes"));
    }
    const char *value = mp_obj_str_get_data(object, len);
    if (memchr(value, '\0', *len) != NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("embedded null character"));
    }
    return value;
}

mp_import_stat_t mp_import_stat(const char *path) {
    char resolved[SOLAR_OS_MICROPYTHON_PATH_MAX];
    struct stat status;
    if (solar_os_micropython_resolve_path(path, resolved, sizeof(resolved)) != 0 ||
        stat(resolved, &status) != 0) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    if (S_ISDIR(status.st_mode)) {
        return MP_IMPORT_STAT_DIR;
    }
    return S_ISREG(status.st_mode) ? MP_IMPORT_STAT_FILE : MP_IMPORT_STAT_NO_EXIST;
}

static void solar_os_file_print(const mp_print_t *print,
                                mp_obj_t self_in,
                                mp_print_kind_t kind) {
    (void)kind;
    const solar_os_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<io.%s %d>", mp_obj_get_type_str(self_in), self->fd);
}

static mp_uint_t solar_os_file_read(mp_obj_t self_in,
                                    void *buffer,
                                    mp_uint_t size,
                                    int *errcode) {
    solar_os_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    solar_os_file_check_open(self);
    solar_os_file_check_cancel();

    const size_t request = size > SOLAR_OS_FILE_IO_CHUNK ? SOLAR_OS_FILE_IO_CHUNK : size;
    ssize_t result;
    do {
        MP_THREAD_GIL_EXIT();
        result = read(self->fd, buffer, request);
        MP_THREAD_GIL_ENTER();
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        *errcode = errno;
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)result;
}

static mp_uint_t solar_os_file_write(mp_obj_t self_in,
                                     const void *buffer,
                                     mp_uint_t size,
                                     int *errcode) {
    solar_os_file_obj_t *self = MP_OBJ_TO_PTR(self_in);
    solar_os_file_check_open(self);

    size_t written = 0;
    while (written < size) {
        solar_os_file_check_cancel();
        size_t request = size - written;
        if (request > SOLAR_OS_FILE_IO_CHUNK) {
            request = SOLAR_OS_FILE_IO_CHUNK;
        }

        ssize_t result;
        do {
            MP_THREAD_GIL_EXIT();
            result = write(self->fd, (const byte *)buffer + written, request);
            MP_THREAD_GIL_ENTER();
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            if (written != 0) {
                return written;
            }
            *errcode = errno;
            return MP_STREAM_ERROR;
        }
        if (result == 0) {
            break;
        }
        written += (size_t)result;
    }
    return written;
}

static mp_uint_t solar_os_file_ioctl(mp_obj_t self_in,
                                     mp_uint_t request,
                                     uintptr_t arg,
                                     int *errcode) {
    solar_os_file_obj_t *self = MP_OBJ_TO_PTR(self_in);

    switch (request) {
        case MP_STREAM_FLUSH: {
            solar_os_file_check_open(self);
            solar_os_file_check_cancel();
            int result;
            do {
                MP_THREAD_GIL_EXIT();
                result = fsync(self->fd);
                MP_THREAD_GIL_ENTER();
            } while (result < 0 && errno == EINTR);
            if (result < 0) {
                *errcode = errno;
                return MP_STREAM_ERROR;
            }
            return 0;
        }
        case MP_STREAM_SEEK: {
            solar_os_file_check_open(self);
            solar_os_file_check_cancel();
            struct mp_stream_seek_t *seek = (struct mp_stream_seek_t *)arg;
            MP_THREAD_GIL_EXIT();
            off_t result = lseek(self->fd, seek->offset, seek->whence);
            MP_THREAD_GIL_ENTER();
            if (result == (off_t)-1) {
                *errcode = errno;
                return MP_STREAM_ERROR;
            }
            seek->offset = result;
            return 0;
        }
        case MP_STREAM_CLOSE:
            if (self->fd >= 0) {
                int result;
                do {
                    MP_THREAD_GIL_EXIT();
                    result = close(self->fd);
                    MP_THREAD_GIL_ENTER();
                } while (result < 0 && errno == EINTR);
                self->fd = -1;
                if (result < 0) {
                    *errcode = errno;
                    return MP_STREAM_ERROR;
                }
            }
            return 0;
        case MP_STREAM_GET_FILENO:
            solar_os_file_check_open(self);
            return self->fd;
        default:
            *errcode = MP_EINVAL;
            return MP_STREAM_ERROR;
    }
}

static const mp_rom_map_elem_t solar_os_file_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto), MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline), MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek), MP_ROM_PTR(&mp_stream_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_tell), MP_ROM_PTR(&mp_stream_tell_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush), MP_ROM_PTR(&mp_stream_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__), MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(solar_os_file_locals, solar_os_file_locals_table);

static const mp_stream_p_t solar_os_fileio_stream = {
    .read = solar_os_file_read,
    .write = solar_os_file_write,
    .ioctl = solar_os_file_ioctl,
};

static const mp_stream_p_t solar_os_textio_stream = {
    .read = solar_os_file_read,
    .write = solar_os_file_write,
    .ioctl = solar_os_file_ioctl,
    .is_text = true,
};

MP_DEFINE_CONST_OBJ_TYPE(
    solar_os_type_fileio,
    MP_QSTR_FileIO,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    print, solar_os_file_print,
    protocol, &solar_os_fileio_stream,
    locals_dict, &solar_os_file_locals
    );

MP_DEFINE_CONST_OBJ_TYPE(
    solar_os_type_textio,
    MP_QSTR_TextIOWrapper,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    print, solar_os_file_print,
    protocol, &solar_os_textio_stream,
    locals_dict, &solar_os_file_locals
    );

typedef struct {
    int flags;
    bool binary;
} solar_os_open_mode_t;

static solar_os_open_mode_t solar_os_file_parse_mode(mp_obj_t mode_in) {
    size_t mode_len;
    const char *mode = solar_os_file_string(mode_in, &mode_len);
    unsigned base_count = 0;
    unsigned plus_count = 0;
    unsigned binary_count = 0;
    unsigned text_count = 0;
    char base = '\0';

    for (size_t index = 0; index < mode_len; index++) {
        switch (mode[index]) {
            case 'r':
            case 'w':
            case 'a':
            case 'x':
                base = mode[index];
                base_count++;
                break;
            case '+':
                plus_count++;
                break;
            case 'b':
                binary_count++;
                break;
            case 't':
                text_count++;
                break;
            default:
                mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
        }
    }
    if (base_count != 1 || plus_count > 1 || binary_count > 1 || text_count > 1 ||
        (binary_count != 0 && text_count != 0)) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid mode"));
    }

    const bool update = plus_count != 0;
    int flags = update ? O_RDWR : O_RDONLY;
    if (base == 'w') {
        flags = (update ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
    } else if (base == 'a') {
        flags = (update ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
    } else if (base == 'x') {
        flags = (update ? O_RDWR : O_WRONLY) | O_CREAT | O_EXCL;
    }
    return (solar_os_open_mode_t) {
        .flags = flags,
        .binary = binary_count != 0,
    };
}

mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    enum { ARG_file, ARG_mode };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_file, MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_mode, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_r)} },
    };
    mp_arg_val_t parsed[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, args, kwargs,
                     MP_ARRAY_SIZE(allowed_args), allowed_args, parsed);

    size_t path_len;
    const char *path = solar_os_file_string(parsed[ARG_file].u_obj, &path_len);
    (void)path_len;
    const solar_os_open_mode_t mode = solar_os_file_parse_mode(parsed[ARG_mode].u_obj);
    char resolved[SOLAR_OS_MICROPYTHON_PATH_MAX];
    if (solar_os_micropython_resolve_path(path, resolved, sizeof(resolved)) != 0) {
        mp_raise_OSError_with_filename(errno, path);
    }

    const mp_obj_type_t *type = mode.binary ? &solar_os_type_fileio : &solar_os_type_textio;
    solar_os_file_obj_t *self = mp_obj_malloc_with_finaliser(solar_os_file_obj_t, type);
    self->fd = -1;

    solar_os_file_check_cancel();
    int fd;
    do {
        MP_THREAD_GIL_EXIT();
        fd = open(resolved, mode.flags, 0666);
        MP_THREAD_GIL_ENTER();
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        mp_raise_OSError_with_filename(errno, path);
    }

    self->fd = fd;
    return MP_OBJ_FROM_PTR(self);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);
