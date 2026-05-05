#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <utility>
#include <vector>

template <typename T> class ObjectPool {
public:
  explicit ObjectPool(std::size_t capacity)
      : slots_(std::make_unique<Slot[]>(capacity)), capacity_(capacity) {
    free_indices_.reserve(capacity_);
    for (std::size_t i = 0; i < capacity_; ++i) {
      free_indices_.push_back(capacity_ - 1 - i);
    }
  }

  ObjectPool(const ObjectPool &) = delete;
  ObjectPool &operator=(const ObjectPool &) = delete;

  ~ObjectPool() {
    for (std::size_t i = 0; i < capacity_; ++i) {
      if (slots_[i].engaged) {
        std::destroy_at(ptr_at(i));
      }
    }
  }

  template <typename... Args> T *acquire(Args &&...args) {
    if (free_indices_.empty()) {
      return nullptr;
    }

    const std::size_t index = free_indices_.back();
    free_indices_.pop_back();
    T *ptr = ptr_at(index);
    std::construct_at(ptr, std::forward<Args>(args)...);
    slots_[index].engaged = true;
    return ptr;
  }

  bool release(T *object) {
    if (object == nullptr || !owns(object)) {
      return false;
    }

    const std::size_t index = index_of(object);
    if (!slots_[index].engaged) {
      return false;
    }

    std::destroy_at(object);
    slots_[index].engaged = false;
    free_indices_.push_back(index);
    return true;
  }

  std::size_t capacity() const { return capacity_; }
  std::size_t available() const { return free_indices_.size(); }

private:
  struct Slot {
    alignas(T) std::byte storage[sizeof(T)];
    bool engaged = false;
  };

  T *ptr_at(std::size_t index) {
    return std::launder(reinterpret_cast<T *>(slots_[index].storage));
  }

  const T *ptr_at(std::size_t index) const {
    return std::launder(reinterpret_cast<const T *>(slots_[index].storage));
  }

  bool owns(const T *object) const {
    if (capacity_ == 0) {
      return false;
    }

    const auto *begin = reinterpret_cast<const std::byte *>(ptr_at(0));
    const auto *end =
        reinterpret_cast<const std::byte *>(ptr_at(capacity_ - 1)) + sizeof(T);
    const auto *raw = reinterpret_cast<const std::byte *>(object);
    return raw >= begin && raw < end;
  }

  std::size_t index_of(const T *object) const {
    const auto *base = reinterpret_cast<const std::byte *>(ptr_at(0));
    const auto *raw = reinterpret_cast<const std::byte *>(object);
    const auto distance = raw - base;
    return static_cast<std::size_t>(distance / sizeof(Slot));
  }

  std::unique_ptr<Slot[]> slots_;
  std::size_t capacity_ = 0;
  std::vector<std::size_t> free_indices_;
};