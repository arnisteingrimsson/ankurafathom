#pragma once
#include <cstdint>
#include <stdexcept>

namespace ankurafathom::rng {
// Unbiased choice from [0, bound), including a full 32-bit outcome space.
// A caller-owned addressed word source makes retries explicit and replayable.
// Contract: docs/SEMANTICS.md, "ABM networks and bounded message topics".
template<class NextWord>
std::uint64_t uniform_index(std::uint64_t bound,NextWord&& next_word) {
    constexpr std::uint64_t word_space=std::uint64_t{1}<<32;
    if(bound==0 || bound>word_space) throw std::invalid_argument("integer draw bound must be in [1,2^32]");
    const auto threshold=word_space%bound;
    for(;;) {
        const std::uint32_t word=next_word();
        if(word>=threshold) return static_cast<std::uint64_t>(word)%bound;
    }
}
} // namespace ankurafathom::rng
