#pragma once

#include <cstddef>
#include <cstdint>

namespace fathom_bench::allocation {
struct Counts {
    std::uint64_t allocations = 0;
    std::uint64_t requested_bytes = 0;
    bool overflow = false;
};

// Current-thread successful C++ allocation requests. Nested scopes are inclusive.
// This implementation is linked only into instrumentation executables.
class Scope {
public:
    Scope() noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Counts counts() const noexcept { return counts_; }
private:
    friend void record(std::size_t bytes) noexcept;
    Scope* previous_;
    Counts counts_;
};
} // namespace fathom_bench::allocation
