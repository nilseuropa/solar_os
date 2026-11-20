from pathlib import Path
import unittest
from tests.test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class ModelUploadJobTest(unittest.TestCase):
    def test_copied_arguments_progress_stop_completion_and_start_failure(self):
        text = (ROOT / "src/jobs/solar_os_model_upload_job.c").read_text()
        body = text[text.index("#define UPLOAD_STACK"):text.index("const solar_os_job_t solar_os_model_upload_job")]
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_TIMEOUT 4
#define SOLAR_OS_APP_STATIC_SRAM_EXCEPTION(x)
#define SOLAR_OS_MEMORY_EXTERNAL_REQUIRED 0
#define SOLAR_OS_TASK_STOP_WAIT_MS 2000
#define SOLAR_OS_TASK_ROLE_BACKGROUND 1
#define SOLAR_OS_PROCESSOR_UPLOAD_REPLACE 1
#define SOLAR_OS_JOB_RESOURCE_FILE 1
#define SOLAR_OS_JOB_RESOURCE_STREAM 2
#define SOLAR_OS_EVENT_TICK 1
#define pdPASS 1
#define tskIDLE_PRIORITY 0
#define tskNO_AFFINITY -1
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
typedef int portMUX_TYPE;
typedef void *TaskHandle_t;
typedef struct {int unused;} solar_os_context_t;
typedef struct {int type;} solar_os_event_t;
typedef struct {int unused;} solar_os_shell_io_t;
typedef bool (*solar_os_processor_cancel_fn)(void *);
typedef void (*solar_os_processor_progress_fn)(const char *,uint32_t,uint32_t,void *);
static unsigned allocations, generation, completed, calls, deletes;
static bool deny, fail_operation;
static void (*pending_task)(void *);
static void *pending_user;
static char output[1024];
static size_t strlcpy(char *out,const char *in,size_t n)
{size_t len=strlen(in);if(n){size_t k=len<n-1?len:n-1;memcpy(out,in,k);out[k]=0;}return len;}
static bool solar_os_processor_kind_valid(const char *s){return s&&*s&&strlen(s)<12;}
static void *solar_os_memory_calloc(size_t n,size_t size,int kind,const char *tag)
{(void)kind;(void)tag;void *p=calloc(n,size);if(p)allocations++;return p;}
static void solar_os_memory_free(void *p){if(p){assert(allocations);allocations--;free(p);}}
static esp_err_t solar_os_jobs_get_generation(const char *name,uint32_t *out)
{assert(!strcmp(name,"model-upload"));*out=++generation;return 0;}
static esp_err_t solar_os_jobs_mark_stopped(const char *name,uint32_t gen,esp_err_t error)
{assert(!strcmp(name,"model-upload")&&gen==generation);(void)error;completed++;return 0;}
static int solar_os_jobs_note_resource(const char *n,int type,const char *resource,const char *detail)
{assert(n&&type&&resource&&detail);return 0;}
static int solar_os_task_create_pinned_internal(void (*fn)(void *),const char *name,uint32_t size,void *user,int priority,TaskHandle_t *out,int core,int role)
{assert(!strcmp(name,"model-upload")&&size==16384&&priority==1&&core==-1&&role==1);if(deny)return 0;pending_task=fn;pending_user=user;*out=(void *)1;return 1;}
static void solar_os_task_delete_internal(TaskHandle_t task){assert(!task);deletes++;}
static bool solar_os_task_wait_done(TaskHandle_t task,const volatile bool *done,uint32_t ms)
{assert(task==(void *)1&&ms==2000);if(!*done)pending_task(pending_user);assert(*done);return true;}
static solar_os_shell_io_t *solar_os_context_shell_io(solar_os_context_t *ctx){return (void *)ctx;}
static void solar_os_shell_io_printf(solar_os_shell_io_t *io,const char *fmt,...)
{(void)io;va_list a;va_start(a,fmt);vsnprintf(output+strlen(output),sizeof(output)-strlen(output),fmt,a);va_end(a);}
static const char *esp_err_to_name(esp_err_t error){return error?"ERROR":"ESP_OK";}
static esp_err_t solar_os_processor_upload(const char *kind,const char *source,const char *bundle,uint32_t slot,uint32_t flags,
    solar_os_processor_cancel_fn cancel,solar_os_processor_progress_fn progress,void *user,char **json)
{
    assert(!strcmp(kind,"himax")&&!strcmp(source,"camera0")&&!strcmp(bundle,"/sdcard/model/bundle.json")&&slot==4&&flags==1);
    calls++;*json=NULL;
    if(cancel(user))return ESP_ERR_TIMEOUT;
    progress("program",64,128,user);
    if(fail_operation)return ESP_ERR_INVALID_STATE;
    *json=solar_os_memory_calloc(1,3,0,"");assert(*json);memcpy(*json,"{}",3);return 0;
}
''' + body + r'''
int main(void)
{
    solar_os_event_t tick={1};solar_os_context_t ctx={0};
    char kind[]="himax",source[]="camera0",path[]="/sdcard/model/bundle.json";
    char *args[]={"model-upload",kind,source,path,"4","--replace"};
    assert(!start(&ctx,6,args));assert(allocations==1&&!state->done&&state->pending);
    assert(start(&ctx,6,args)==ESP_ERR_INVALID_STATE);
    kind[0]='x';source[0]='x';path[1]='x';
    pending_task(pending_user);assert(calls==1&&state->done&&deletes==1);
    event(&ctx,&tick);assert(completed==1&&!state->pending&&!state->task);
    detail(&ctx);assert(strstr(output,"slot=4")&&strstr(output,"bytes=64/128"));
    kind[0]='h';source[0]='c';path[1]='s';
    assert(!start(&ctx,6,args));stop(&ctx);
    assert(state->done&&!state->pending&&state->error==ESP_ERR_TIMEOUT&&calls==2);
    fail_operation=true;assert(!start(&ctx,6,args));pending_task(pending_user);event(&ctx,&tick);
    assert(state->error==ESP_ERR_INVALID_STATE&&completed==2);
    deny=true;assert(start(&ctx,6,args)==ESP_ERR_NO_MEM&&state->done&&!state->pending);
    args[4]="4294967296";assert(start(&ctx,6,args)==ESP_ERR_INVALID_ARG);
    args[4]="-1";assert(start(&ctx,6,args)==ESP_ERR_INVALID_ARG);
    args[4]="4";args[5]="--other";assert(start(&ctx,6,args)==ESP_ERR_INVALID_ARG);
    solar_os_memory_free(state);state=NULL;assert(!allocations);
}
''')
