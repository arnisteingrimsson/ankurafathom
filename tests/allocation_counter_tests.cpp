#include "allocation_counter.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <new>
#include <stdexcept>
#include <thread>

using fathom_bench::allocation::Counts;
using fathom_bench::allocation::Scope;
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
// A direct, opaque operator call avoids new-expression allocation elision.
void* ordinary(std::size_t bytes) {
    void* (*volatile call)(std::size_t) = ::operator new;
    return call(bytes);
}
void expect(Counts counts, std::uint64_t calls, std::uint64_t bytes) {
    require(!counts.overflow && counts.allocations == calls && counts.requested_bytes == bytes,
            "allocation counts or requested bytes differ from the hand oracle");
}
void forms() {
    void* outside = ordinary(99);
    ::operator delete(outside);
    Scope scope;
    expect(scope.counts(), 0, 0);
    void* a = ordinary(7);
    void* b = ::operator new[](13);
    void* zero = ordinary(0);
    void* c = ::operator new(31, std::align_val_t{64});
    void* d = ::operator new[](37, std::align_val_t{128});
    void* e = ::operator new(11, std::nothrow);
    void* f = ::operator new[](17, std::nothrow);
    void* g = ::operator new(19, std::align_val_t{64}, std::nothrow);
    void* h = ::operator new[](23, std::align_val_t{64}, std::nothrow);
    require(zero && e && f && g && h, "successful nothrow/zero allocation returned null");
    require(reinterpret_cast<std::uintptr_t>(c) % 64 == 0 && reinterpret_cast<std::uintptr_t>(d) % 128 == 0 &&
            reinterpret_cast<std::uintptr_t>(g) % 64 == 0 && reinterpret_cast<std::uintptr_t>(h) % 64 == 0,
            "aligned allocation violated alignment");
    expect(scope.counts(), 9, 158);
    ::operator delete(a, std::size_t{7});
    ::operator delete[](b, std::size_t{13});
    ::operator delete(zero);
    ::operator delete(c, std::size_t{31}, std::align_val_t{64});
    ::operator delete[](d, std::size_t{37}, std::align_val_t{128});
    ::operator delete(e, std::nothrow);
    ::operator delete[](f, std::nothrow);
    ::operator delete(g, std::align_val_t{64}, std::nothrow);
    ::operator delete[](h, std::align_val_t{64}, std::nothrow);
    expect(scope.counts(), 9, 158); // Frees neither increment nor decrement requests.
}
void nesting_and_unwinding() {
    Scope outer;
    {
        Scope inner;
        ::operator delete(ordinary(13));
        expect(inner.counts(), 1, 13);
    }
    ::operator delete(ordinary(7));
    expect(outer.counts(), 2, 20);
    try {
        Scope unwound;
        ::operator delete(ordinary(21));
        throw 1;
    } catch (int) {}
    ::operator delete(ordinary(23));
    expect(outer.counts(), 4, 64);
}
void independent_threads() {
    std::atomic<bool> start{false}, done{false};
    Counts worker_counts;
    // Thread creation occurs outside the parent's measured interval.
    std::thread worker([&] {
        while (!start.load()) std::this_thread::yield();
        {
            Scope scope;
            ::operator delete(ordinary(13));
            worker_counts = scope.counts();
        }
        done.store(true);
    });
    Counts parent_counts;
    {
        Scope scope;
        start.store(true);
        ::operator delete(ordinary(7));
        while (!done.load()) std::this_thread::yield();
        parent_counts = scope.counts();
    }
    worker.join();
    expect(parent_counts, 1, 7);
    expect(worker_counts, 1, 13);
}
}
int main() {
    try {
        forms(); nesting_and_unwinding(); independent_threads();
        std::cout << "Allocation counter forms, byte totals, alignment, scope restoration, and thread isolation pass\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
