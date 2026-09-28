#include "ankurafathom/hybrid/rate_driven_source.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <variant>

namespace {

using Entity = ankurafathom::des::Entity;
using Message = std::variant<Entity, double>;
using Source = ankurafathom::hybrid::RateDrivenSource<Message>;
using Simulator = ankurafathom::devs::Simulator<Message>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

double hazard(std::uint64_t entity_id) {
    const auto word = ankurafathom::rng::draw(123,
        ankurafathom::rng::DrawAddress{2, 3, entity_id, 0, 20, 0})[0];
    return -std::log(ankurafathom::rng::uniform_open(word));
}

void test_hazard_survives_rate_changes() {
    const double h0 = hazard(100);
    const double h1 = hazard(101);
    Simulator simulator;
    const auto source = simulator.add(std::make_unique<Source>(123, 2, 3, 100, 2, 20, 2));
    simulator.inject(0, source, 0, Message{1.0});
    (void)simulator.step();
    simulator.inject(h0 / 2, source, 0, Message{2.0});
    (void)simulator.step();
    const auto first = simulator.step();
    require(first && first->emissions.size() == 1 &&
            std::abs(first->time - 0.75 * h0) < 1e-12 &&
            std::get<Entity>(first->emissions[0].value).id == 100,
            "first arrival failed piecewise hazard oracle");
    const auto service_word = ankurafathom::rng::draw(123,
        ankurafathom::rng::DrawAddress{2, 3, 100, 0, 21, 0})[0];
    require(std::get<Entity>(first->emissions[0].value).service_duration ==
            ankurafathom::rng::exponential(2, service_word),
            "service draw used the wrong Philox address");

    const double pause = first->time + h1 / 8;
    simulator.inject(pause, source, 0, Message{0.0});
    (void)simulator.step();
    require(!std::isfinite(simulator.next_time()), "zero rate did not pause the pending arrival");
    const double resume = pause + 1;
    simulator.inject(resume, source, 0, Message{1.0});
    (void)simulator.step();
    const auto second = simulator.step();
    require(second && second->emissions.size() == 1 &&
            std::abs(second->time - (resume + 0.75 * h1)) < 1e-12 &&
            std::get<Entity>(second->emissions[0].value).id == 101 &&
            !std::isfinite(simulator.next_time()),
            "paused hazard was redrawn, lost, or exceeded the entity cap");
}

void test_arrival_at_rate_update_uses_old_rate() {
    const double h0 = hazard(100);
    Simulator simulator;
    const auto source = simulator.add(std::make_unique<Source>(123, 2, 3, 100, 2, 20, 2));
    simulator.inject(0, source, 0, Message{1.0});
    (void)simulator.step();
    simulator.inject(h0, source, 0, Message{2.0});
    const auto coincident = simulator.step();
    require(coincident && coincident->emissions.size() == 1 &&
            std::get<Entity>(coincident->emissions[0].value).id == 100 &&
            std::abs(coincident->time - h0) < 1e-12,
            "coincident rate update displaced an already-due arrival");
    const auto next = simulator.step();
    require(next && std::abs(next->time - (h0 + hazard(101) / 2)) < 1e-12,
            "post-confluence rate did not apply to the next hazard");
}

} // namespace

int main() {
    try {
        test_hazard_survives_rate_changes();
        test_arrival_at_rate_update_uses_old_rate();
        std::cout << "rate-driven hybrid source tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
