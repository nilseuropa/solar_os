/* SolarOS owns ESP-DL dispatch under its global inference admission lock.
 * Worker storage is lazy and is released after the last backend closes. */
namespace {
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
constexpr size_t kWorkerCount = 2;
constexpr uint32_t kWorkerStackBytes = 2048;
struct DualCoreWorkerRuntime;
struct DualCoreWorkerTaskArgs {
    DualCoreWorkerRuntime *runtime = nullptr;
    size_t slot = 0;
};
struct DualCoreWorkerRuntime {
    StaticSemaphore_t dispatch_mutex_buffer = {};
    SemaphoreHandle_t dispatch_mutex = nullptr;
    StaticSemaphore_t completion_buffer = {};
    SemaphoreHandle_t completion = nullptr;
    StaticTask_t task_buffers[kWorkerCount] = {};
    StackType_t task_stacks[kWorkerCount][kWorkerStackBytes / sizeof(StackType_t)] = {};
    TaskHandle_t task_handles[kWorkerCount] = {};
    DualCoreWorkerTaskArgs task_args[kWorkerCount] = {};
    Module *op = nullptr;
    void *args[kWorkerCount] = {};
    bool stopping = false;
};
static DualCoreWorkerRuntime *worker_runtime;

void DualCoreWorkerTask(void *arg)
{
    auto *task_arg = static_cast<DualCoreWorkerTaskArgs *>(arg);
    auto *runtime = task_arg->runtime;
    const size_t slot = task_arg->slot;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (__atomic_load_n(&runtime->stopping, __ATOMIC_ACQUIRE)) {
            vTaskSuspend(nullptr);
        }
        __sync_synchronize();
        runtime->op->forward_args(runtime->args[slot]);
        xSemaphoreGive(runtime->completion);
    }
}

void DeleteDualCoreWorkerRuntime(DualCoreWorkerRuntime *runtime)
{
    if (!runtime) return;
    /* No dispatch is active. Suspend each worker before deleting it, so its
     * caller-owned stack/TCB can never be freed while the other CPU uses it. */
    __atomic_store_n(&runtime->stopping, true, __ATOMIC_RELEASE);
    for (auto task : runtime->task_handles) if (task) xTaskNotifyGive(task);
    for (auto task : runtime->task_handles) {
        if (!task) continue;
        while (eTaskGetState(task) != eSuspended) vTaskDelay(1);
        vTaskDelete(task);
    }
    if (runtime->completion) vSemaphoreDelete(runtime->completion);
    if (runtime->dispatch_mutex) vSemaphoreDelete(runtime->dispatch_mutex);
    runtime->~DualCoreWorkerRuntime();
    heap_caps_free(runtime);
}

DualCoreWorkerRuntime &GetDualCoreWorkerRuntime()
{
    if (worker_runtime) return *worker_runtime;
    void *storage = heap_caps_calloc(1, sizeof(DualCoreWorkerRuntime),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!storage) throw std::bad_alloc();
    auto *runtime = new (storage) DualCoreWorkerRuntime();
    runtime->dispatch_mutex = xSemaphoreCreateMutexStatic(&runtime->dispatch_mutex_buffer);
    runtime->completion = xSemaphoreCreateCountingStatic(kWorkerCount, 0, &runtime->completion_buffer);
    bool ready = runtime->dispatch_mutex && runtime->completion;
    for (size_t i = 0; ready && i < kWorkerCount; ++i) {
        runtime->task_args[i] = {runtime, i};
        runtime->task_handles[i] = xTaskCreateStaticPinnedToCore(DualCoreWorkerTask,
            i == 0 ? "dl_mc0" : "dl_mc1", kWorkerStackBytes, &runtime->task_args[i],
            tskIDLE_PRIORITY + 1, runtime->task_stacks[i], &runtime->task_buffers[i], i);
        ready = runtime->task_handles[i] != nullptr;
    }
    if (!ready) {
        DeleteDualCoreWorkerRuntime(runtime);
        throw std::bad_alloc();
    }
    worker_runtime = runtime;
    return *runtime;
}
#endif
} // namespace

void module_workers_release()
{
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    DeleteDualCoreWorkerRuntime(worker_runtime);
    worker_runtime = nullptr;
#endif
}

void module_forward_dual_core(Module *op, void *args1, void *args2)
{
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    auto &runtime = GetDualCoreWorkerRuntime();
    xSemaphoreTake(runtime.dispatch_mutex, portMAX_DELAY);
    while (xSemaphoreTake(runtime.completion, 0) == pdTRUE) {}
    const UBaseType_t priority = uxTaskPriorityGet(xTaskGetCurrentTaskHandle());
    for (auto task : runtime.task_handles) vTaskPrioritySet(task, priority);
    runtime.op = op;
    runtime.args[0] = args1;
    runtime.args[1] = args2;
    __sync_synchronize();
    for (auto task : runtime.task_handles) xTaskNotifyGive(task);
    for (size_t i = 0; i < kWorkerCount; ++i) xSemaphoreTake(runtime.completion, portMAX_DELAY);
    runtime.op = nullptr;
    runtime.args[0] = runtime.args[1] = nullptr;
    xSemaphoreGive(runtime.dispatch_mutex);
#else
    op->forward_args(args1);
    op->forward_args(args2);
#endif
}
