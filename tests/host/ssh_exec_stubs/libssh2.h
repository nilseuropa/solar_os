#pragma once

#include <stddef.h>
#include <sys/types.h>

typedef struct ssh_session LIBSSH2_SESSION;
typedef struct ssh_channel LIBSSH2_CHANNEL;
typedef void libssh2_cb_generic(void);
#define LIBSSH2_ERROR_EAGAIN (-37)
#define LIBSSH2_CALLBACK_SEND 5
#define LIBSSH2_CALLBACK_RECV 6
#define LIBSSH2_RECV_FUNC(name) ssize_t name(int socket, void *buffer, size_t length, int flags, void **abstract)
#define LIBSSH2_SEND_FUNC(name) ssize_t name(int socket, const void *buffer, size_t length, int flags, void **abstract)

LIBSSH2_CHANNEL *libssh2_channel_open_session(LIBSSH2_SESSION *session);
int libssh2_session_last_errno(LIBSSH2_SESSION *session);
int libssh2_channel_exec(LIBSSH2_CHANNEL *channel, const char *command);
ssize_t libssh2_channel_read(LIBSSH2_CHANNEL *channel, char *bytes, size_t capacity);
ssize_t libssh2_channel_read_stderr(LIBSSH2_CHANNEL *channel, char *bytes, size_t capacity);
ssize_t libssh2_channel_write(LIBSSH2_CHANNEL *channel, const char *bytes, size_t size);
int libssh2_channel_eof(LIBSSH2_CHANNEL *channel);
int libssh2_channel_send_eof(LIBSSH2_CHANNEL *channel);
int libssh2_channel_wait_closed(LIBSSH2_CHANNEL *channel);
int libssh2_channel_free(LIBSSH2_CHANNEL *channel);
int libssh2_channel_get_exit_status(LIBSSH2_CHANNEL *channel);
int libssh2_channel_get_exit_signal(LIBSSH2_CHANNEL *channel, char **signal, size_t *signal_size,
    char **message, size_t *message_size, char **lang, size_t *lang_size);
libssh2_cb_generic *libssh2_session_callback_set2(LIBSSH2_SESSION *session, int type, libssh2_cb_generic *callback);
