module objstore.tcp.detail;

import plexdb.os;
import plexdb.arena;

namespace objstore::tcp {
    // ========================================================================
    // chunk chain
    // ========================================================================
    bool append_chunk_chain(ChunkChain& chain, U32 buffer_idx, U8* buffer_data, int byte_count) {
        // small chunk
        if (!full(chain.small_chunks)) {
            ChunkNode& node = emplace_back(chain.small_chunks);
            node.value.buffer_idx = buffer_idx;
            node.value.data = { buffer_data, byte_count };
            
            push_back(chain.chunks, &node);

            // @note buffer_idx is in use
            return true; 
        }
        
        // large chunk
        if (chain.arena == nullptr) {
            chain.arena = arena::allocate(LARGE_BUFFER_SIZE, nullptr);
        }
        
        U8* large_buffer = arena::push_array_no_zero<U8>(&chain.arena, byte_count);
        os::memory_copy(large_buffer, buffer_data, byte_count);

        ChunkNode* node = arena::push_array_no_zero<ChunkNode>(&chain.arena, 1);
        node->value.buffer_idx = -1;
        node->value.data = { large_buffer, byte_count };
        
        push_back(chain.chunks, node);

        // @note buffer_idx is NOT in use
        return false;
    }

    void release_chunk_chain(DynamicArray<BufferInfo>& buffer_infos, ChunkChain& chain) {
        for (Chunk& chunk : chain.chunks) {
            if (chunk.buffer_idx >= 0) {
                buffer_infos[chunk.buffer_idx].client = os::zero_handle();
            }
        }

        // @note accumulates MAX_CONNECTIONS x ARENA_PAGE_SIZE memory eventually
        if (chain.arena != nullptr) {
            arena::clear(chain.arena);
        }
        clear(chain.chunks);
        clear(chain.small_chunks);
    }

    // ========================================================================
    // printing
    // ========================================================================
    AutoString8 to_str(const Stats& s) {
        AutoString8 res = "=== Stats ===\n"_as;
        res += "Total connections: " + plexdb::to_str(s.total_connections) + "\n";
        res += "Active connections: " + plexdb::to_str(s.active_connections) + "\n";
        res += "Total bytes read: " + plexdb::to_str(s.total_bytes_read) + "(" + plexdb::to_str((F64)s.total_bytes_read / 1_mb) + "MB)\n";
        res += "Total bytes written: " + plexdb::to_str(s.total_bytes_written) + "(" + plexdb::to_str((F64)s.total_bytes_written / 1_mb) + "MB)\n";
        res += "=============";
        return res;
    }
}