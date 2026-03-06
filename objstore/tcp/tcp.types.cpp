module objstore.tcp.types;

namespace objstore::tcp {
    ChunkChain::~ChunkChain() {
        if (this->arena != nullptr) {
            deallocate(this->arena);
        }
    }
}