#include "ankurafathom/rng/philox.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_published_vectors() {
    using ankurafathom::rng::philox4x32_10;
    require(philox4x32_10({0, 0, 0, 0}, {0, 0}) ==
            std::array<std::uint32_t, 4>{0x6627e8d5U, 0xe169c58dU, 0xbc57ac4cU, 0x9b00dbd8U},
            "Random123 zero-vector KAT failed");
    require(philox4x32_10({0xffffffffU, 0xffffffffU, 0xffffffffU, 0xffffffffU},
                          {0xffffffffU, 0xffffffffU}) ==
            std::array<std::uint32_t, 4>{0x408f276dU, 0x41c83b0eU, 0xa20bc7c6U, 0x6d5451fdU},
            "Random123 all-ones KAT failed");
    require(philox4x32_10({0x243f6a88U, 0x85a308d3U, 0x13198a2eU, 0x03707344U},
                          {0xa4093822U, 0x299f31d0U}) ==
            std::array<std::uint32_t, 4>{0xd16cfe09U, 0x94fdccebU, 0x5001e420U, 0x24126ea1U},
            "Random123 mixed-vector KAT failed");
}

void test_address_packing_and_limits() {
    using ankurafathom::rng::DrawAddress;
    using ankurafathom::rng::draw;
    using ankurafathom::rng::pack_counter;
    const DrawAddress maximal{65535, 65535, 0xFFFFFFFFFFFFULL, 65535, 65535, 65535};
    require(pack_counter(maximal) == std::array<std::uint32_t, 4>{0xffffffffU, 0xffffffffU,
                                                                  0xffffffffU, 0xffffffffU},
            "maximal address packing is wrong");
    const DrawAddress origin{};
    const auto first = draw(123, origin);
    require(first == draw(123, origin), "same seed and address must produce identical words");
    DrawAddress next = origin;
    next.draw_index = 1;
    require(pack_counter(next) != pack_counter(origin), "draw indices must have distinct counters");
    next = origin;
    next.entity = 1;
    require(pack_counter(next) != pack_counter(origin), "entity IDs must have distinct counters");
    next = origin;
    next.stream = 1;
    require(pack_counter(next) != pack_counter(origin), "streams must have distinct counters");
    next = origin;
    next.entity = 0x1000000000000ULL;
    bool caught = false;
    try { (void)pack_counter(next); } catch (const std::out_of_range&) { caught = true; }
    require(caught, "address overflow must be rejected");
}

void test_uniform_and_bernoulli_boundaries() {
    using ankurafathom::rng::uniform_open;
    using ankurafathom::rng::bernoulli;
    require(uniform_open(0) > 0 && uniform_open(0xffffffffU) < 1,
            "uniform transform must exclude exact zero and one");
    require(!bernoulli(0, 0) && bernoulli(1, 0xffffffffU),
            "Bernoulli boundary probabilities are wrong");
    bool caught = false;
    try { (void)bernoulli(1.1, 0); } catch (const std::invalid_argument&) { caught = true; }
    require(caught, "invalid Bernoulli probability must be rejected");
}

void test_exponential_transform() {
    using ankurafathom::rng::exponential;
    require(std::abs(exponential(2, 0x7fffffffU) +
                     std::log(0.5 - 1.0 / 8589934592.0) / 2) < 1e-15,
            "exponential inverse-CDF reference point is wrong");
    require(exponential(2, 0xffffffffU) > 0 &&
            exponential(2, 0) > exponential(2, 0xffffffffU),
            "exponential tail endpoints are wrong");
    bool caught = false;
    try { (void)exponential(0, 0); } catch (const std::invalid_argument&) { caught = true; }
    require(caught, "nonpositive exponential rate must be rejected");
}

} // namespace

int main() {
    try {
        test_published_vectors();
        test_address_packing_and_limits();
        test_uniform_and_bernoulli_boundaries();
        test_exponential_transform();
        std::cout << "Philox tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
