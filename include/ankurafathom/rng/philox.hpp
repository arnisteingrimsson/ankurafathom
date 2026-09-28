#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace ankurafathom::rng {

using Counter = std::array<std::uint32_t, 4>;
using Key = std::array<std::uint32_t, 2>;

// Philox4x32-10, matching the Random123 published known-answer vectors.
inline Counter philox4x32_10(Counter counter, Key key) noexcept {
    constexpr std::uint32_t multiplier0 = 0xD2511F53U;
    constexpr std::uint32_t multiplier1 = 0xCD9E8D57U;
    constexpr std::uint32_t bump0 = 0x9E3779B9U;
    constexpr std::uint32_t bump1 = 0xBB67AE85U;
    for (int round = 0; round < 10; ++round) {
        const std::uint64_t product0 = static_cast<std::uint64_t>(multiplier0) * counter[0];
        const std::uint64_t product1 = static_cast<std::uint64_t>(multiplier1) * counter[2];
        const std::uint32_t hi0 = static_cast<std::uint32_t>(product0 >> 32);
        const std::uint32_t lo0 = static_cast<std::uint32_t>(product0);
        const std::uint32_t hi1 = static_cast<std::uint32_t>(product1 >> 32);
        const std::uint32_t lo1 = static_cast<std::uint32_t>(product1);
        counter = {static_cast<std::uint32_t>(hi1 ^ counter[1] ^ key[0]), lo1,
                   static_cast<std::uint32_t>(hi0 ^ counter[3] ^ key[1]), lo0};
        if (round != 9) {
            key[0] += bump0;
            key[1] += bump1;
        }
    }
    return counter;
}

struct DrawAddress {
    std::uint32_t scenario = 0;
    std::uint32_t replication = 0;
    std::uint64_t entity = 0;
    std::uint32_t step = 0;
    std::uint32_t stream = 0;
    std::uint32_t draw_index = 0;
};

inline Counter pack_counter(const DrawAddress& address) {
    constexpr std::uint32_t max16 = 0xFFFFU;
    constexpr std::uint64_t max48 = 0xFFFFFFFFFFFFULL;
    if (address.scenario > max16 || address.replication > max16 ||
        address.entity > max48 || address.step > max16 ||
        address.stream > max16 || address.draw_index > max16)
        throw std::out_of_range("Philox draw address exceeds its declared bit width");
    return {static_cast<std::uint32_t>(address.entity),
            static_cast<std::uint32_t>((address.entity >> 32) | (static_cast<std::uint64_t>(address.stream) << 16)),
            static_cast<std::uint32_t>(address.scenario | (address.replication << 16)),
            static_cast<std::uint32_t>(address.step | (address.draw_index << 16))};
}

inline Counter draw(std::uint64_t seed, const DrawAddress& address) {
    const Key key{static_cast<std::uint32_t>(seed), static_cast<std::uint32_t>(seed >> 32)};
    return philox4x32_10(pack_counter(address), key);
}

inline double uniform_open(std::uint32_t word) noexcept {
    return (static_cast<double>(word) + 0.5) / 4294967296.0;
}

inline bool bernoulli(double probability, std::uint32_t word) {
    if (!std::isfinite(probability) || probability < 0 || probability > 1)
        throw std::invalid_argument("Bernoulli probability must be in [0,1]");
    return uniform_open(word) < probability;
}

// Inverse-CDF exponential variate with mean 1/rate; the open uniform avoids log(0).
inline double exponential(double rate, std::uint32_t word) {
    if (!std::isfinite(rate) || rate <= 0)
        throw std::invalid_argument("exponential rate must be finite and positive");
    return -std::log(uniform_open(word)) / rate;
}

} // namespace ankurafathom::rng
