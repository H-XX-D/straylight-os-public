// bin/pmem/allocator.h
// Free-list allocator for persistent memory regions
#pragma once

#include <straylight/result.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace straylight::pmem {

class PmemAllocator {
public:
    /// Initialize the allocator over a memory region.
    Result<void, std::string> init(void* base, size_t size);

    /// Allocate bytes with given alignment. Returns pointer within the managed region.
    Result<void*, std::string> alloc(size_t bytes, size_t align = 64);

    /// Free a previously allocated pointer.
    Result<void, std::string> free(void* ptr);

    [[nodiscard]] size_t used() const { return used_; }
    [[nodiscard]] size_t available() const { return total_ - used_ - metadata_overhead_; }

private:
    // Free-list node stored inline in the managed memory.
    struct FreeBlock {
        size_t size;        // Total block size including this header.
        FreeBlock* next;    // Next free block, or nullptr.
    };

    static constexpr size_t BLOCK_HEADER_SIZE = sizeof(FreeBlock);
    static constexpr size_t MIN_BLOCK_SIZE = 64; // Minimum allocation unit.

    void* base_ = nullptr;
    size_t total_ = 0;
    size_t used_ = 0;
    size_t metadata_overhead_ = 0;
    FreeBlock* free_list_ = nullptr;

    /// Coalesce adjacent free blocks.
    void coalesce();

    /// Align a value up to the given alignment.
    static size_t align_up(size_t val, size_t alignment);
};

} // namespace straylight::pmem
