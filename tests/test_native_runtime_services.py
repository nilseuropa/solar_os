from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class NativeRuntimeServicesTest(unittest.TestCase):
    def test_mutex_condition_tls_lifetimes_and_cooperative_sleep(self):
        source = (ROOT / 'src/services/solar_os_native.c').read_text()
        names = ('native_time_sleep_ms', 'native_thread_mutex_create',
                 'native_thread_mutex_lock', 'native_thread_mutex_unlock',
                 'native_thread_mutex_destroy', 'native_thread_condition_create',
                 'native_thread_condition_wait', 'native_thread_condition_broadcast',
                 'native_thread_condition_destroy', 'native_thread_key_create',
                 'native_thread_key_delete', 'native_thread_key_get', 'native_thread_key_set')
        functions = production_functions(source, names)
        harness = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
static atomic_int allocations;
static bool fail_alloc;
#define SOLAR_OS_MEMORY_INTERNAL_CRITICAL 1
static void *solar_os_memory_alloc(size_t bytes,int cls,const char *tag)
{ assert(cls==1 && tag);if(fail_alloc)return NULL;void *p=malloc(bytes);if(p)allocations++;return p; }
static void solar_os_memory_free(void *p) { if(p)allocations--;free(p); }
typedef uint32_t TickType_t;
#define configTICK_RATE_HZ 100U
#define portMAX_DELAY UINT32_MAX
static uint32_t slept;
static void vTaskDelay(uint32_t ticks) { slept=ticks; }
''' + functions + r'''
static uint32_t key;
static atomic_int destroyed;
static int parent, child, ready;
static void *mutex,*condition;
static void destructor(void *value) { assert(value==&child);destroyed++; }
static void *worker(void *arg)
{
    (void)arg;assert(!native_thread_key_get(key));
    assert(!native_thread_key_set(key,&child));
    assert(!native_thread_mutex_lock(mutex));ready=1;
    assert(!native_thread_condition_broadcast(condition));
    assert(!native_thread_mutex_unlock(mutex));
    assert(native_thread_key_get(key)==&child);return NULL;
}
int main(void)
{
    native_time_sleep_ms(0);assert(slept==0);
    native_time_sleep_ms(1);assert(slept==1);
    native_time_sleep_ms(11);assert(slept==2);
    native_time_sleep_ms(UINT32_MAX);assert(slept==429496730U);
    void *value=(void*)1;
    fail_alloc=true;assert(native_thread_mutex_create(&value,0)==ENOMEM && !value);
    assert(native_thread_condition_create(&value)==ENOMEM && !value);fail_alloc=false;
    assert(native_thread_mutex_create(NULL,0)==EINVAL);
    assert(native_thread_mutex_create(&value,2)==EINVAL);
    assert(!native_thread_mutex_create(&value,1));
    assert(!native_thread_mutex_lock(value));assert(!native_thread_mutex_lock(value));
    assert(native_thread_mutex_destroy(value)==EBUSY && allocations==1);
    assert(!native_thread_mutex_unlock(value));assert(!native_thread_mutex_unlock(value));
    assert(!native_thread_mutex_destroy(value));assert(allocations==0);
    assert(!native_thread_key_create(&key,destructor));
    assert(!native_thread_key_set(key,&parent));
    assert(!native_thread_mutex_create(&mutex,0));
    assert(!native_thread_condition_create(&condition));
    assert(!native_thread_mutex_lock(mutex));
    pthread_t task;assert(!pthread_create(&task,NULL,worker,NULL));
    while(!ready)assert(!native_thread_condition_wait(condition,mutex));
    assert(!native_thread_mutex_unlock(mutex));assert(!pthread_join(task,NULL));
    assert(destroyed==1 && native_thread_key_get(key)==&parent);
    assert(!native_thread_key_delete(key));assert(destroyed==1);
    assert(!native_thread_condition_destroy(condition));
    assert(!native_thread_mutex_destroy(mutex));assert(allocations==0);
    assert(native_thread_mutex_lock(NULL)==EINVAL);
    assert(native_thread_condition_wait(NULL,NULL)==EINVAL);
    puts("native runtime services passed");
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'runtime'
            subprocess.run(['cc','-x','c','-std=c11','-include','stdbool.h',
                            '-Wall','-Wextra','-Werror','-pthread','-o',str(binary),'-'],
                           input=harness,text=True,check=True)
            subprocess.run([str(binary)],check=True,timeout=10)

    def test_public_runtime_headers_compile_as_c_and_cpp(self):
        headers = ('solar_os_native_libc_abi.h','solar_os_native_thread_abi.h',
                   'solar_os_native_time_abi.h','solar_os_inference_backend_abi.h')
        source = ''.join(f'#include "{header}"\n' for header in headers)
        for compiler,language in [('cc','c'),('c++','c++')]:
            subprocess.run([compiler,'-x',language,'-Wall','-Wextra','-Werror','-pedantic',
                            '-I',str(ROOT/'include'),'-fsyntax-only','-'],
                           input=source,text=True,check=True)
