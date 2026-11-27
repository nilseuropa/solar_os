#pragma once
#include <stdint.h>

#define SOLAR_OS_NATIVE_THREAD_SERVICE "thread"
#define SOLAR_OS_NATIVE_THREAD_ABI 1U

/* Functions return POSIX error numbers. Mutexes and keys are provider-owned;
 * destroy/delete all of them before unloading code used by TLS destructors.
 * Clear values on every task using a key before deleting it. TLS destructors
 * run on task exit, never on key deletion. Their task-exit cleanup must finish
 * while a lease still protects the destructor's code. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*mutex_create)(void **mutex, uint32_t recursive);
    int (*mutex_lock)(void *mutex);
    int (*mutex_unlock)(void *mutex);
    int (*mutex_destroy)(void *mutex);
    int (*key_create)(uint32_t *key, void (*destructor)(void *));
    int (*key_delete)(uint32_t key);
    void *(*key_get)(uint32_t key);
    int (*key_set)(uint32_t key, const void *value);
    int (*condition_create)(void **condition);
    int (*condition_wait)(void *condition, void *mutex);
    int (*condition_broadcast)(void *condition);
    int (*condition_destroy)(void *condition);
    /* Optional joinable workers. entry must return; join waits until its code
     * has stopped executing and releases its internal stack/TCB. Keep a library
     * lease through join. core is 0/1; stack_bytes is 2048..16384. entry must
     * clear private TLS values before return. join cannot run on that worker. */
    int (*worker_start)(void **worker, void (*entry)(void *), void *argument,
                        uint32_t stack_bytes, uint32_t core);
    int (*worker_join)(void *worker);
} solar_os_native_thread_api_v1_t;
