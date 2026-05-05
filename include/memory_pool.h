#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

class MemoryPool {
public:
  static constexpr std::size_t kMaxAlignment = 4096;

  explicit MemoryPool(std::size_t capacity_bytes)
      : capacity_bytes_(capacity_bytes),
        buffer_(std::make_unique<std::byte[]>(capacity_bytes + kMaxAlignment)) {
  }

  MemoryPool(const MemoryPool &) = delete;
  MemoryPool &operator=(const MemoryPool &) = delete;

  void *acquire(std::size_t bytes,
                std::size_t alignment = alignof(std::max_align_t)) {
    if (bytes == 0 || alignment == 0 || alignment > kMaxAlignment) {
      return nullptr;
    }

    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(buffer_.get());
    const std::uintptr_t current = base + offset_;
    const std::uintptr_t aligned = align_up(current, alignment);
    const std::size_t aligned_offset = static_cast<std::size_t>(aligned - base);

    if (aligned_offset > capacity_bytes_ ||
        bytes > capacity_bytes_ - aligned_offset) {
      return nullptr;
    }

    void *ptr = reinterpret_cast<void *>(aligned);
    offset_ = aligned_offset + bytes;
    return ptr;
  }

  void reset() { offset_ = 0; }

  std::size_t capacity() const { return capacity_bytes_; }
  std::size_t used() const { return offset_; }
  std::size_t remaining() const { return capacity_bytes_ - offset_; }

private:
  static std::uintptr_t align_up(std::uintptr_t value, std::size_t alignment) {
    const std::uintptr_t mask = static_cast<std::uintptr_t>(alignment - 1);
    return (value + mask) & ~mask;
  }

  std::size_t capacity_bytes_ = 0;
  std::size_t offset_ = 0;
  std::unique_ptr<std::byte[]> buffer_;
};