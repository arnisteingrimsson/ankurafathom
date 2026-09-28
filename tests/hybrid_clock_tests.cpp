#include "ankurafathom/hybrid/clocked_sd.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {

using ankurafathom::devs::Atomic;
using ankurafathom::devs::Input;
using ankurafathom::devs::PortValue;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class PulseSource final : public Atomic<double> {
public:
    double time_advance() const override {
        return sent_ < 2 ? 0.5 : std::numeric_limits<double>::infinity();
    }
    std::vector<PortValue<double>> output() const override {
        return {{0, sent_ == 0 ? 5.0 : 2.0}};
    }
    void internal_transition() override { ++sent_; }
    void external_transition(double, const std::vector<Input<double>>&) override {
        throw std::logic_error("pulse source received input");
    }
    void confluent_transition(const std::vector<Input<double>>&) override {
        throw std::logic_error("pulse source received confluent input");
    }
private:
    int sent_ = 0;
};

void test_pulse_between_and_on_ticks() {
    ankurafathom::sd::Model stocks;
    const auto stock = stocks.add_stock("stock", 0);
    stocks.add_flow(ankurafathom::sd::Model::boundary, stock,
                    [](const auto&, double) { return 1.0; });
    auto clocked = std::make_unique<ankurafathom::hybrid::ClockedSD<double>>(std::move(stocks), 1);
    auto* observed = clocked.get();
    ankurafathom::devs::Simulator<double> simulator;
    const auto source = simulator.add(std::make_unique<PulseSource>());
    const auto target = simulator.add(std::move(clocked));
    simulator.connect(source, 0, target, static_cast<std::uint32_t>(stock));

    const auto first = simulator.step();
    require(first && first->time == 0.5 && observed->model().state()[stock] == 5.5,
            "off-tick pulse must apply after integration to its timestamp");
    const auto second = simulator.step();
    require(second && second->time == 1 && observed->model().state()[stock] == 8,
            "pulse on a tick must apply after the interval ending at that tick");
    const auto third = simulator.step();
    require(third && third->time == 2 && observed->model().state()[stock] == 9,
            "stock must continue from post-pulse committed state");
}

void test_fractional_tick_grid_alignment() {
    ankurafathom::sd::Model stocks;
    const auto stock = stocks.add_stock("stock", 0);
    stocks.add_flow(ankurafathom::sd::Model::boundary, stock,
                    [](const auto&, double) { return 1.0; });
    auto clocked = std::make_unique<ankurafathom::hybrid::ClockedSD<double>>(std::move(stocks), 0.1);
    auto* observed = clocked.get();
    ankurafathom::devs::Simulator<double> simulator;
    (void)simulator.add(std::move(clocked));
    for (int step = 1; step <= 100; ++step) {
        const double time = static_cast<double>(step) * 0.1;
        (void)simulator.run_until(time);
        require(observed->time() == time,
                "fractional SD tick must equal the observation-grid timestamp");
        require(std::abs(observed->model().state()[stock] - time) < 1e-12,
                "fractional SD integration missed an observation tick");
    }
}

} // namespace

int main() {
    try {
        test_pulse_between_and_on_ticks();
        test_fractional_tick_grid_alignment();
        std::cout << "hybrid clock tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
