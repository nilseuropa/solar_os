/* Execute the real overlay with concurrent task/notification/semaphore stubs. */
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <new>
#include <thread>
#include <cstdio>

#ifndef CONFIG_FREERTOS_NUMBER_OF_CORES
#define CONFIG_FREERTOS_NUMBER_OF_CORES 2
#endif
using UBaseType_t = unsigned;
using StackType_t = uint32_t;
enum State { eRunning, eBlocked, eSuspended };
struct Deleted {};
struct Task {
    std::mutex mutex;
    std::condition_variable condition;
    std::thread thread;
    State state = eRunning;
    bool deleted = false;
    unsigned notifications = 0;
};
using TaskHandle_t = Task *;
struct StaticTask_t { char bytes[64]; };
struct StaticSemaphore_t {
    std::mutex mutex;
    std::condition_variable condition;
    unsigned count = 0, maximum = 0;
};
using SemaphoreHandle_t = StaticSemaphore_t *;
constexpr unsigned pdTRUE = 1, portMAX_DELAY = ~0U, tskIDLE_PRIORITY = 0;
constexpr unsigned MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2;
static thread_local Task *current_task;
static int allocations, tasks, semaphores, fail_task = -1, fail_semaphore = -1;
static bool fail_storage;
void *heap_caps_calloc(size_t n, size_t size, unsigned caps)
{
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (fail_storage) return nullptr;
    void *p = calloc(n, size); if (p) ++allocations; return p;
}
void heap_caps_free(void *p) { if (p) --allocations; free(p); }
void vTaskDelay(unsigned) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
TaskHandle_t xTaskGetCurrentTaskHandle() { return current_task; }
unsigned uxTaskPriorityGet(TaskHandle_t) { return 1; }
void vTaskPrioritySet(TaskHandle_t, unsigned) {}
TaskHandle_t xTaskCreateStaticPinnedToCore(void (*fn)(void *), const char *, unsigned stack,
    void *arg, unsigned, StackType_t *, StaticTask_t *, unsigned core)
{
    assert(stack == 2048 && core < 2);
    if (fail_task == 0) return nullptr;
    if (fail_task > 0) --fail_task;
    Task *t = new Task(); ++tasks;
    t->thread = std::thread([=] { current_task = t; try { fn(arg); } catch (Deleted &) {} });
    return t;
}
void xTaskNotifyGive(TaskHandle_t t)
{
    std::lock_guard<std::mutex> lock(t->mutex); ++t->notifications; t->condition.notify_one();
}
unsigned ulTaskNotifyTake(unsigned, unsigned)
{
    Task *t = current_task; std::unique_lock<std::mutex> lock(t->mutex);
    t->state = eBlocked;
    t->condition.wait(lock, [=] { return t->notifications || t->deleted; });
    if (t->deleted) throw Deleted();
    unsigned n = t->notifications; t->notifications = 0; t->state = eRunning; return n;
}
void vTaskSuspend(TaskHandle_t)
{
    Task *t = current_task; std::unique_lock<std::mutex> lock(t->mutex); t->state = eSuspended;
    t->condition.wait(lock, [=] { return t->deleted; }); throw Deleted();
}
State eTaskGetState(TaskHandle_t t) { std::lock_guard<std::mutex> lock(t->mutex); return t->state; }
void vTaskDelete(TaskHandle_t t)
{
    { std::lock_guard<std::mutex> lock(t->mutex); assert(t->state == eSuspended);
      t->deleted = true; t->condition.notify_one(); }
    t->thread.join(); delete t; --tasks;
}
SemaphoreHandle_t xSemaphoreCreateCountingStatic(unsigned maximum, unsigned initial, StaticSemaphore_t *s)
{
    if (fail_semaphore == 0) return nullptr;
    if (fail_semaphore > 0) --fail_semaphore;
    s->count = initial; s->maximum = maximum; ++semaphores; return s;
}
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s)
{ return xSemaphoreCreateCountingStatic(1, 1, s); }
unsigned xSemaphoreTake(SemaphoreHandle_t s, unsigned timeout)
{
    std::unique_lock<std::mutex> lock(s->mutex);
    if (!s->count && !timeout) return 0;
    s->condition.wait(lock, [=] { return s->count > 0; }); --s->count; return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t s)
{
    std::lock_guard<std::mutex> lock(s->mutex); assert(s->count < s->maximum);
    ++s->count; s->condition.notify_one();
}
void vSemaphoreDelete(SemaphoreHandle_t) { --semaphores; }

namespace dl { namespace module {
class Module { public: virtual void forward_args(void *) = 0; };
#include "../../scripts/espdl/worker_runtime.inc"
} }
class Counter : public dl::module::Module {
public:
    void forward_args(void *value) override {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
        ++*static_cast<std::atomic<int> *>(value);
    }
};
static void empty() { assert(!allocations && !tasks && !semaphores); }
int main()
{
    Counter op; std::atomic<int> a{0}, b{0};
    using namespace dl::module;
    empty(); module_workers_release(); empty();
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
    for (int kind = 0; kind < 5; ++kind) {
        fail_storage = kind == 0;
        fail_semaphore = kind == 1 ? 0 : kind == 2 ? 1 : -1;
        fail_task = kind == 3 ? 0 : kind == 4 ? 1 : -1;
        bool failed = false;
        try { module_forward_dual_core(&op, &a, &b); } catch (std::bad_alloc &) { failed = true; }
        assert(failed); empty(); module_workers_release(); empty();
    }
    fail_storage = false; fail_semaphore = fail_task = -1;
#endif
    for (int cycle = 0; cycle < 100; ++cycle) {
        for (int run = 0; run < 5; ++run) {
            module_forward_dual_core(&op, &a, &b);
            assert(a == cycle * 5 + run + 1 && b == a);
#if CONFIG_FREERTOS_NUMBER_OF_CORES > 1
            assert(allocations == 1 && tasks == 2 && semaphores == 2);
#else
            empty();
#endif
        }
        module_workers_release(); empty(); module_workers_release(); empty();
    }
    puts("ESP-DL lazy workers: OOM/partial creation/retry/join/reuse/100 releases passed");
}
