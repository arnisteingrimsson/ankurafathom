#include "allocation_counter.hpp"

#include <cstdlib>
#include <limits>
#include <new>

namespace fathom_bench::allocation {
namespace { thread_local Scope* active = nullptr; }
Scope::Scope() noexcept : previous_(active) { active = this; }
Scope::~Scope() { active = previous_; }
void record(std::size_t bytes) noexcept {
    for (auto* scope = active; scope; scope = scope->previous_) {
        auto& counts = scope->counts_;
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (counts.allocations == maximum) counts.overflow = true;
        else ++counts.allocations;
        if (bytes > maximum - counts.requested_bytes) {
            counts.requested_bytes = maximum;
            counts.overflow = true;
        } else counts.requested_bytes += bytes;
    }
}
} // namespace fathom_bench::allocation

namespace {
void* allocate(std::size_t requested, std::size_t alignment = 0) {
    const auto size = requested == 0 ? 1 : requested;
    for (;;) {
        void* pointer = nullptr;
        if (alignment == 0) pointer = std::malloc(size);
        else if (posix_memalign(&pointer, alignment, size) != 0) pointer = nullptr;
        if (pointer) {
            fathom_bench::allocation::record(requested);
            return pointer;
        }
        const auto handler = std::get_new_handler();
        if (!handler) throw std::bad_alloc();
        handler();
    }
}
}

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, std::align_val_t alignment) { return allocate(size, static_cast<std::size_t>(alignment)); }
void* operator new[](std::size_t size, std::align_val_t alignment) { return allocate(size, static_cast<std::size_t>(alignment)); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new[](size); } catch (...) { return nullptr; }
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new(size, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new[](size, alignment); } catch (...) { return nullptr; }
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { std::free(pointer); }
