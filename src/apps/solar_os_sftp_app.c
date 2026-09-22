/*
 * FTP and SFTP deliberately share one file-manager implementation so their
 * panes, operation keys, progress, prompts, and connection form stay aligned.
 */
#include "solar_os_identity.h"
#include "solar_os_sftp.h"

#define SOLAR_OS_FTP_APP_SFTP 1
#define FTP_APP_COMMAND "sftp"
#define FTP_APP_PROTOCOL "SFTP"
#define FTP_APP_TAG "solar_os_sftp_app"
#define FTP_APP_MEMORY_TAG "sftp.app.entries"
#define FTP_APP_TEMP_PREFIX ".sftp-view-"
#define FTP_APP_TASK_NAME "sftp_app"
#define FTP_APP_SUMMARY "two-pane SFTP file manager"

#define SOLAR_OS_FTP_DEFAULT_PORT SOLAR_OS_SFTP_DEFAULT_PORT
#define SOLAR_OS_FTP_NAME_MAX SOLAR_OS_SFTP_NAME_MAX
#define SOLAR_OS_FTP_REPLY_MAX SOLAR_OS_SFTP_ERROR_MAX
#define SOLAR_OS_FTP_DEFAULT_TIMEOUT_MS SOLAR_OS_SFTP_DEFAULT_TIMEOUT_MS
#define solar_os_ftp_session_t solar_os_sftp_session_t
#define solar_os_ftp_options_t solar_os_sftp_options_t
#define solar_os_ftp_entry_t solar_os_sftp_entry_t
#define solar_os_ftp_connect solar_os_sftp_connect
#define solar_os_ftp_disconnect solar_os_sftp_disconnect
#define solar_os_ftp_last_reply solar_os_sftp_last_error
#define solar_os_ftp_list solar_os_sftp_list
#define solar_os_ftp_download solar_os_sftp_download
#define solar_os_ftp_upload solar_os_sftp_upload
#define solar_os_ftp_mkdir solar_os_sftp_mkdir
#define solar_os_ftp_rmdir solar_os_sftp_rmdir
#define solar_os_ftp_remove solar_os_sftp_remove
#define solar_os_ftp_rename solar_os_sftp_rename
#define solar_os_ftp_app solar_os_sftp_app

#include "solar_os_ftp_app.c"
