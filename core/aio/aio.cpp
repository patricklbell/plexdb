module;
#include "macros.h"
#include <coroutine>

module plexdb.aio;

import plexdb.base;
import plexdb.os;
import plexdb.coroutine;

namespace plexdb::aio {
    coroutine::Task<Error> file_read(FileIOContext& ctx, os::Handle file, U64 offset, U32 count, U8* dst) {
        if (!ctx.read || !ctx.sync) {
            co_return Error::Invalid;
        }
        co_return co_await (*ctx.read)(file, offset, count, dst);
    }

    coroutine::Task<Error> file_write(FileIOContext& ctx, os::Handle file, U64 offset, U32 count, const U8* src) {
        if (!ctx.write || !ctx.sync) {
            co_return Error::Invalid;
        }
        co_return co_await (*ctx.write)(file, offset, count, src);
    }

    coroutine::Task<Error> file_sync(FileIOContext& ctx, os::Handle file) {
        if (!ctx.sync) {
            co_return Error::Invalid;
        }
        co_return co_await (*ctx.sync)(file);
    }
}