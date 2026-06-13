module;
#include <plexdb/macros/macros.h>
#if PLEXDB_OS_LINUX
#include <pthread.h>
#include <semaphore.h>
#include <sched.h>
#include <time.h>
#include <stdlib.h>
#endif

module plexdb.os.thread;

namespace plexdb::os {

#if PLEXDB_OS_LINUX
    // ========================================================================
    // thread
    // ========================================================================
    struct ThreadEntity {
        pthread_t      pthread;
        ThreadFunction fn;
        void*          arg;
    };

    static void* thread_entry(void* raw) {
        auto* entity = static_cast<ThreadEntity*>(raw);
        entity->fn(entity->arg);
        return nullptr;
    }

    Handle thread_launch(ThreadFunction fn, void* arg) {
        auto* entity = static_cast<ThreadEntity*>(malloc(sizeof(ThreadEntity)));
        entity->fn   = fn;
        entity->arg  = arg;
        pthread_create(&entity->pthread, nullptr, thread_entry, entity);
        Handle h;
        h.u64[0] = reinterpret_cast<U64>(entity);
        return h;
    }

    void thread_join(Handle handle) {
        auto* entity = reinterpret_cast<ThreadEntity*>(handle.u64[0]);
        pthread_join(entity->pthread, nullptr);
        free(entity);
    }

    void thread_detach(Handle handle) {
        auto* entity = reinterpret_cast<ThreadEntity*>(handle.u64[0]);
        pthread_detach(entity->pthread);
        free(entity);
    }

    void thread_set_name(Handle handle, const char* name) {
        auto* entity = reinterpret_cast<ThreadEntity*>(handle.u64[0]);
        pthread_setname_np(entity->pthread, name);
    }

    void thread_set_affinity(Handle handle, U32 core_id) {
        auto*     entity = reinterpret_cast<ThreadEntity*>(handle.u64[0]);
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(core_id, &cpuset);
        pthread_setaffinity_np(entity->pthread, sizeof(cpu_set_t), &cpuset);
    }

    Handle thread_current() {
        // @note returns a handle that should not be joined/freed
        Handle h;
        h.u64[0] = reinterpret_cast<U64>(nullptr);
        return h;
    }

    // ========================================================================
    // semaphore - linux
    // ========================================================================
    Handle semaphore_open(U32 initial_count) {
        auto* sem = static_cast<sem_t*>(malloc(sizeof(sem_t)));
        sem_init(sem, 0, initial_count);
        Handle h;
        h.u64[0] = reinterpret_cast<U64>(sem);
        return h;
    }

    void semaphore_close(Handle handle) {
        auto* sem = reinterpret_cast<sem_t*>(handle.u64[0]);
        sem_destroy(sem);
        free(sem);
    }

    void semaphore_signal(Handle handle) {
        auto* sem = reinterpret_cast<sem_t*>(handle.u64[0]);
        sem_post(sem);
    }

    void semaphore_wait(Handle handle) {
        auto* sem = reinterpret_cast<sem_t*>(handle.u64[0]);
        sem_wait(sem);
    }

    // ========================================================================
    // sleep - linux
    // ========================================================================
    void sleep_ms(U32 ms) {
        struct timespec ts;
        ts.tv_sec  = ms / 1000;
        ts.tv_nsec = (ms % 1000) * 1000000L;
        nanosleep(&ts, nullptr);
    }
#endif
}
