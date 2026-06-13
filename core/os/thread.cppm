export module plexdb.os.thread;

import plexdb.base;
import plexdb.os.core;

export namespace plexdb::os {
    using ThreadFunction = void (*)(void*);

    Handle thread_launch(ThreadFunction fn, void* arg);
    void   thread_join(Handle handle);
    void   thread_detach(Handle handle);
    void   thread_set_name(Handle handle, const char* name);
    void   thread_set_affinity(Handle handle, U32 core_id);
    Handle thread_current();

    Handle semaphore_open(U32 initial_count);
    void   semaphore_close(Handle handle);
    void   semaphore_signal(Handle handle);
    void   semaphore_wait(Handle handle);

    void sleep_ms(U32 ms);
}
