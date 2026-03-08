module;
#include "macros.h"

module objstore.http;

import plexdb.base;

import objstore.tcp;
import objstore.engine;

using namespace plexdb;

namespace objstore::http {
    // @todo async
    // HttpChunkedEncodingMetadata create_http_chunked_encoding(U64 buffer_size) {
    //     return {
    //         .buffer_size = buffer_size,
    //         .prefix_max_byte_count = static_cast<U32>(fmt_length("%" PRIx64 "\r\n", buffer_size)),
    //         .suffix = "\r\n",
    //     };
    // }

    // HttpChunkedEncodingFlushFunctor::HttpChunkedEncodingFlushFunctor(tcp::Connection* in_connection, tcp::Chunk* in_chunk, HttpChunkedEncodingMetadata* in_meta, tcp::AsyncWriteFunctor* in_write)
    //     : connection(in_connection)
    //     , chunk(in_chunk)
    //     , meta(in_meta)
    //     , write(in_write)
    // {
    //     assert_true(in_chunk->buffer_idx >= 0, "cannot use large chunk for buffering");
    // }

    // void HttpChunkedEncodingFlushFunctor::operator ()(const char* data, U64 length, bool is_final) {
    //     const auto& prefix_max_byte_count = this->meta->prefix_max_byte_count;
    //     const auto& suffix = this->meta->suffix;

    //     const U64 usable_buffer_size = this->chunk->data.length - (prefix_max_byte_count + suffix.length);

    //     assert_true(this->chunk != nullptr, "chunk chain cannot be empty while flushing http response");
    //     assert_true(length <= usable_buffer_size, "flush too large for http buffer, this should not happen!");
    //     assert_true(reinterpret_cast<const U8*>(data) == this->chunk->data.ptr + prefix_max_byte_count, "flush data ptr does not match beginning of http chunk");

    //     // write prefix directly at start of reserved space
    //     U32 prefix_byte_count;
    //     {
    //         String8 prefix_view{reinterpret_cast<char*>(this->chunk->data.ptr), prefix_max_byte_count};
    //         fmt_raw(prefix_view, "%" PRIx64 "\r\n", length);
    //         prefix_byte_count = prefix_view.length;
    //         assert_true(prefix_byte_count <= prefix_max_byte_count, "http chunk prefix too long");
    //     }
        
    //     // calculate offset for right-alignment and move prefix to start
    //     U32 first_byte_idx = prefix_max_byte_count - prefix_byte_count;
    //     os::memory_shift_right(TArrayView(this->chunk->data.ptr, prefix_byte_count), first_byte_idx);
        
    //     // write suffix at the end
    //     os::memory_copy(this->chunk->data.ptr + prefix_max_byte_count + length, suffix.data, suffix.length);

    //     if (is_final) {
    //         const String8 final{"0\r\n\r\n"};

    //         // cannot fit final chunk into current buffer
    //         if (prefix_byte_count + length + suffix.length + final.length > this->chunk->data.length) {
    //             (*this->write)(this->connection, this->chunk->buffer_idx, first_byte_idx, prefix_byte_count + length + suffix.length);
    //             length = 0;
    //             first_byte_idx = 0;
    //             os::memory_copy(this->chunk->data.ptr, final.data, final.length);
    //             (*this->write)(this->connection, this->chunk->buffer_idx, 0, final.length);
    //             return;
    //         }

    //         os::memory_copy(this->chunk->data.ptr + prefix_max_byte_count + length + suffix.length, final.data, final.length);
    //     }
    //     (*this->write)(this->connection, this->chunk->buffer_idx, first_byte_idx, prefix_byte_count + length + suffix.length);
    // }

    int get_http_status_for_execution_status(engine::ExecutionStatus status) {
        switch (status) {
            case engine::ExecutionStatus::Success:        return 200;
            case engine::ExecutionStatus::ServerError:    return 500;
            case engine::ExecutionStatus::SyntaxError:    return 400;
            case engine::ExecutionStatus::Unauthorized:   return 403;
            case engine::ExecutionStatus::Invalid:        return 400;
            case engine::ExecutionStatus::ConfigError:    return 400;
            case engine::ExecutionStatus::AlreadyExists:  return 409;
            case engine::ExecutionStatus::NotImplemented: return 501;
        }
        return 500;
    }

    const char* get_reason_for_status(int code) {
        switch (code) {
            case 200: return "OK";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 500: return "Internal Server Error";
            case 501: return "Not Implemented";
            case 502: return "Bad Gateway";
            case 503: return "Service Unavailable";
            default:  return "Unknown";
        }
    }

    const char* get_connection_header(bool close) {
        if (close) {
            return "Connection: close\r\n";
        }
        return "Connection: keep-alive\r\n";
    }
}
