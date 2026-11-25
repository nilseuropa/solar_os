from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions
ROOT=Path(__file__).resolve().parents[1]

class NativeHttpLifecycleTest(unittest.TestCase):
    def test_strict_close_retains_stream_until_worker_join(self):
        source=(ROOT/'src/services/solar_os_http_stream.c').read_text()
        functions=production_functions(source,('http_stream_close_common',
            'solar_os_http_stream_close','solar_os_http_stream_close_wait'))
        harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define HTTP_STREAM_STOP_WAIT_MS 2000U
#define SOLAR_OS_HTTP_STREAM_MAX_TIMEOUT_MS 60000U
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static int http_stream_lock;
typedef struct {
    void *request,*task;
    volatile bool task_done;
    bool detached,stop_requested;
} solar_os_http_stream_t;
typedef struct { solar_os_http_stream_t *streams[2]; } solar_os_http_stream_session_t;
static unsigned cancelled,destroyed,waited;
static solar_os_http_stream_t *http_stream_find(solar_os_http_stream_session_t *session,uint32_t id,size_t *index)
{ *index=0;return id==1?session->streams[0]:NULL; }
static int solar_os_http_request_cancel(void *request) { assert(request);++cancelled;return ESP_OK; }
static bool solar_os_task_wait_done(void *task,const volatile bool *done,uint32_t timeout)
{ assert(task && timeout<=60000);++waited;return *done; }
static void http_stream_destroy(solar_os_http_stream_t *stream) { assert(stream->task_done);++destroyed; }
''' + functions + r'''
int main(void) {
    solar_os_http_stream_t worker={.request=(void *)1,.task=(void *)2};
    solar_os_http_stream_session_t session={{&worker,NULL}};
    assert(solar_os_http_stream_close_wait(&session,1,42)==ESP_ERR_TIMEOUT);
    assert(session.streams[0]==&worker && worker.stop_requested && !worker.detached && !destroyed);
    assert(solar_os_http_stream_close_wait(&session,1,60001)==ESP_ERR_INVALID_ARG);
    assert(!worker.detached && session.streams[0]==&worker);
    worker.task_done=true;
    assert(!solar_os_http_stream_close_wait(&session,1,42));
    assert(!session.streams[0] && destroyed==1 && cancelled==2 && waited==2);
    assert(solar_os_http_stream_close_wait(&session,1,0)==ESP_ERR_NOT_FOUND);
    /* Existing callers retain the original detach-on-timeout behavior. */
    worker=(solar_os_http_stream_t){.request=(void *)1,.task=(void *)2};
    session.streams[0]=&worker;
    assert(solar_os_http_stream_close(&session,1)==ESP_ERR_TIMEOUT);
    assert(!session.streams[0] && worker.detached && worker.stop_requested && destroyed==1);
    worker.task_done=true;http_stream_destroy(&worker);
    assert(destroyed==2);return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'close'
            subprocess.run(['cc','-x','c','-std=c11','-Wall','-Wextra','-Werror',
                '-fsanitize=address,undefined','-fno-pie','-no-pie',
                '-I',str(ROOT/'tests/host'),'-o',str(binary),'-'],input=harness,text=True,check=True)
            subprocess.run([str(binary)],check=True)
