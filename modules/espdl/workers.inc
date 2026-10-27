/* Dispatch is serialized by inference admission. Workers finish all private
 * TLS cleanup before host join permits releasing their code and stack. */
extern "C" {
#include "solar_os_native_thread_abi.h"
const solar_os_native_thread_api_v1_t *solar_os_espdl_port_threads(void);
void solar_os_espdl_port_leave_task(void);
}
namespace {
struct LibraryWorkers {
    void *mutex = nullptr, *condition = nullptr, *tasks[2] = {};
    struct Argument { LibraryWorkers *owner; unsigned slot; } arguments[2] = {};
    Module *op = nullptr;
    void *args[2] = {};
    unsigned generation = 0, completed = 0;
    bool stopping = false, failed = false;
};
static LibraryWorkers *workers;
static void LibraryWorker(void *value)
{
    auto *arg = static_cast<LibraryWorkers::Argument *>(value);
    auto *w = arg->owner;
    const auto *t = solar_os_espdl_port_threads();
    unsigned generation = 0;
    t->mutex_lock(w->mutex);
    for (;;) {
        while (!w->stopping && generation == w->generation)
            t->condition_wait(w->condition, w->mutex);
        if (w->stopping) break;
        generation = w->generation;
        Module *op = w->op; void *args = w->args[arg->slot];
        t->mutex_unlock(w->mutex);
        bool failed = false;
        try { op->forward_args(args); } catch (...) { failed = true; }
        solar_os_espdl_port_leave_task();
        t->mutex_lock(w->mutex);
        w->failed |= failed; ++w->completed;
        t->condition_broadcast(w->condition);
    }
    t->mutex_unlock(w->mutex);
    solar_os_espdl_port_leave_task();
}
static void DeleteLibraryWorkers(LibraryWorkers *w)
{
    if (!w) return;
    const auto *t = solar_os_espdl_port_threads();
    if (w->mutex) {
        t->mutex_lock(w->mutex); w->stopping = true;
        if (w->condition) t->condition_broadcast(w->condition);
        t->mutex_unlock(w->mutex);
    }
    for (auto task : w->tasks) if (task) t->worker_join(task);
    if (w->condition) t->condition_destroy(w->condition);
    if (w->mutex) t->mutex_destroy(w->mutex);
    delete w;
}
static LibraryWorkers *GetLibraryWorkers()
{
    if (workers) return workers;
    const auto *t = solar_os_espdl_port_threads();
    auto *w = new LibraryWorkers;
    bool ready = !t->mutex_create(&w->mutex, 0) && !t->condition_create(&w->condition);
    for (unsigned i = 0; ready && i < 2; ++i) {
        w->arguments[i] = {w, i};
        ready = !t->worker_start(&w->tasks[i], LibraryWorker, &w->arguments[i], 4096, i);
    }
    if (!ready) { DeleteLibraryWorkers(w); throw std::bad_alloc(); }
    workers = w; return w;
}
}
void module_workers_release()
{ DeleteLibraryWorkers(workers); workers = nullptr; }
void module_forward_dual_core(Module *op, void *args1, void *args2)
{
    auto *w = GetLibraryWorkers();
    const auto *t = solar_os_espdl_port_threads();
    t->mutex_lock(w->mutex);
    w->op = op; w->args[0] = args1; w->args[1] = args2;
    w->completed = 0; w->failed = false; ++w->generation;
    t->condition_broadcast(w->condition);
    while (w->completed < 2) t->condition_wait(w->condition, w->mutex);
    const bool failed = w->failed;
    w->op = nullptr; w->args[0] = w->args[1] = nullptr;
    t->mutex_unlock(w->mutex);
    if (failed) throw std::bad_alloc();
}
