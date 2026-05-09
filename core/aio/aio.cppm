module;
#include "macros.h"
#include <coroutine>

export module plexdb.aio;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;

using namespace plexdb;

export namespace plexdb::aio {
    // ========================================================================
    // Error codes
    // ========================================================================
    enum class Error { None, IO, Invalid, Cancelled };

    // ========================================================================
    // Event loop interface — same shape for both contexts
    // ========================================================================
    using SubmitFunctor = AutoFunctor<void()>;
    using DrainFunctor  = AutoFunctor<U32()>;
    using WakeFdFunctor = AutoFunctor<os::Handle()>;

    // ========================================================================
    // File I/O context
    // ========================================================================
    using FileReadFunctor  = AutoFunctor<coroutine::Task<Error>(os::Handle, U64 offset, U32 count, U8* dst)>;
    using FileWriteFunctor = AutoFunctor<coroutine::Task<Error>(os::Handle, U64 offset, U32 count, const U8* src)>;
    using FileSyncFunctor  = AutoFunctor<coroutine::Task<Error>(os::Handle)>;

    struct FileIOContext {
        FileReadFunctor*  read;
        FileWriteFunctor* write;
        FileSyncFunctor*  sync;
        SubmitFunctor*    submit;
        DrainFunctor*     drain;
        WakeFdFunctor*   wake_fd;
    };

    coroutine::Task<Error> file_read (FileIOContext& ctx, os::Handle file, U64 offset, U32 count, U8* dst);
    coroutine::Task<Error> file_write(FileIOContext& ctx, os::Handle file, U64 offset, U32 count, const U8* src);
    coroutine::Task<Error> file_sync (FileIOContext& ctx, os::Handle file);

    // ========================================================================
    // Network I/O context
    //   @note event loop facing only
    // ========================================================================
    struct NetworkIOContext {
        SubmitFunctor*   submit;
        DrainFunctor*    drain;
        WakeFdFunctor*   wake_fd;
    };
}
