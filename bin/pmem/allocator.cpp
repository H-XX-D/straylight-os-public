// bin/pmem/allocator.cpp
#include "allocator.h"

#include <algorithm>
#include <cstring>

namespace straylight::pmem {

size_t PmemAllocator::align_up(size_t val, size_t alignment) {
    return (val + alignment - 1) & ~(alignment - 1);
}

Result<void, std::string> PmemAllocator::init(void* base, size_t size) {
    if (!base) {
        return Result<void, std::string>::error("Base pointer is null");
    }
    if (size < MIN_BLOCK_SIZE * 2) {
        return Result<void, std::string>::error(
            "Region too small: need at least " + std::to_string(MIN_BLOCK_SIZE * 2) + " bytes");
    }

    base_ = base;
    total_ = size;
    used_ = 0;
    metadata_overhead_ = 0;

    // Initialize the entire region as one free block.
    free_list_ = reinterpret_cast<FreeBlock*>(base);
    free_list_->size = size;
    free_list_->next = nullptr;

    return Result<void, std::string>::ok();
}

Result<void*, std::string> PmemAllocator::alloc(size_t bytes, size_t align) {
    if (bytes == 0) {
        return Result<void*, std::string>::error("Cannot allocate zero bytes");
    }
    if (!base_) {
        return Result<void*, std::string>::error("Allocator not initialized");
    }

    // We need space for: header + alignment padding + user data.
    // The header is stored just before the user pointer.
    size_t needed = align_up(bytes, std::max(align, static_cast<size_t>(8)));
    size_t total_needed = BLOCK_HEADER_SIZE + needed;
    total_needed = std::max(total_needed, MIN_BLOCK_SIZE);

    // First-fit search through free list.
    FreeBlock* prev = nullptr;
    FreeBlock* curr = free_list_;

    while (curr) {
        if (curr->size >= total_needed) {
            // Check alignment of the user pointer (after header).
            auto user_addr = reinterpret_cast<uintptr_t>(curr) + BLOCK_HEADER_SIZE;
            size_t padding = align_up(user_addr, align) - user_addr;
            size_t actual_needed = total_needed + padding;

            if (curr->size >= actual_needed) {
                // Split if remainder is large enough for a new free block.
                size_t remainder = curr->size - actual_needed;
                if (remainder >= MIN_BLOCK_SIZE) {
                    auto* new_free = reinterpret_cast<FreeBlock*>(
                        reinterpret_cast<uint8_t*>(curr) + actual_needed);
                    new_free->size = remainder;
                    new_free->next = curr->next;

                    if (prev) {
                        prev->next = new_free;
                    } else {
                        free_list_ = new_free;
                    }
                    curr->size = actual_needed;
                } else {
                    // Use the whole block.
                    actual_needed = curr->size;
                    if (prev) {
                        prev->next = curr->next;
                    } else {
                        free_list_ = curr->next;
                    }
                }

                used_ += actual_needed;
                metadata_overhead_ += BLOCK_HEADER_SIZE + padding;

                void* user_ptr = reinterpret_cast<void*>(
                    align_up(reinterpret_cast<uintptr_t>(curr) + BLOCK_HEADER_SIZE, align));

                // Store the block start address just before the aligned user pointer
                // so free() can find the header.
                // We write the original block pointer into the header.
                curr->next = nullptr; // Mark as allocated.

                return Result<void*, std::string>::ok(user_ptr);
            }
        }
        prev = curr;
        curr = curr->next;
    }

    return Result<void*, std::string>::error(
        "Out of persistent memory: requested " + std::to_string(bytes) +
        " bytes, available ~" + std::to_string(available()));
}

Result<void, std::string> PmemAllocator::free(void* ptr) {
    if (!ptr) {
        return Result<void, std::string>::error("Cannot free null pointer");
    }
    if (!base_) {
        return Result<void, std::string>::error("Allocator not initialized");
    }

    auto ptr_u = reinterpret_cast<uintptr_t>(ptr);
    auto base_u = reinterpret_cast<uintptr_t>(base_);

    if (ptr_u < base_u || ptr_u >= base_u + total_) {
        return Result<void, std::string>::error("Pointer not within managed region");
    }

    // Find the block header. It's the closest BLOCK_HEADER_SIZE-aligned address
    // before the user pointer that falls within our region.
    // We search backwards from the user pointer for the header.
    auto* header = reinterpret_cast<FreeBlock*>(
        reinterpret_cast<uint8_t*>(ptr) - BLOCK_HEADER_SIZE);

    // Validate: header must be within our region and have a reasonable size.
    auto header_u = reinterpret_cast<uintptr_t>(header);
    if (header_u < base_u) {
        // Try the block start if there was alignment padding.
        // Walk back to find a plausible header.
        header = reinterpret_cast<FreeBlock*>(
            reinterpret_cast<uint8_t*>(ptr) - BLOCK_HEADER_SIZE);
    }

    if (header->size == 0 || header->size > total_) {
        return Result<void, std::string>::error("Corrupt block header or double free");
    }

    size_t block_size = header->size;
    used_ -= block_size;

    // Insert into free list in address order for coalescing.
    FreeBlock* prev = nullptr;
    FreeBlock* curr = free_list_;
    while (curr && reinterpret_cast<uintptr_t>(curr) < header_u) {
        prev = curr;
        curr = curr->next;
    }

    header->next = curr;
    if (prev) {
        prev->next = header;
    } else {
        free_list_ = header;
    }

    coalesce();
    return Result<void, std::string>::ok();
}

void PmemAllocator::coalesce() {
    FreeBlock* curr = free_list_;
    while (curr && curr->next) {
        auto* next_expected = reinterpret_cast<FreeBlock*>(
            reinterpret_cast<uint8_t*>(curr) + curr->size);
        if (next_expected == curr->next) {
            // Merge curr and curr->next.
            curr->size += curr->next->size;
            curr->next = curr->next->next;
            // Don't advance; check if we can merge with the new next too.
        } else {
            curr = curr->next;
        }
    }
}

} // namespace straylight::pmem
