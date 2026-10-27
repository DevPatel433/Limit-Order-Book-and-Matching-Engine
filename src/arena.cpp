#include "lob/arena.hpp"

namespace lob {

void SlabArena::refill() {
    // Any tail left in the old chunk (< the request size, at most kMaxPooledBytes) is abandoned.
    auto* header = static_cast<ChunkHeader*>(::operator new(kChunkBytes));
    header->next = chunk_list_;
    chunk_list_ = header;
    ++chunks_;
    cur_ = reinterpret_cast<char*>(header) + sizeof(ChunkHeader);
    end_ = reinterpret_cast<char*>(header) + kChunkBytes;
}

SlabArena::~SlabArena() {
    for (ChunkHeader* c = chunk_list_; c;) {
        ChunkHeader* next = c->next;
        ::operator delete(c);
        c = next;
    }
}

}  // namespace lob
