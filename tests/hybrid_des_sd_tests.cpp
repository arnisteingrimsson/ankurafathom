#include "ankurafathom/des/single_server.hpp"
#include "ankurafathom/hybrid/clocked_sd.hpp"
#include "ankurafathom/hybrid/entity_to_pulse.hpp"

#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <variant>
#include <vector>

namespace {

using Entity = ankurafathom::des::Entity;
using Message = std::variant<Entity, double>;
using Input = ankurafathom::devs::Input<Message>;
using PortValue = ankurafathom::devs::PortValue<Message>;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Source final : public ankurafathom::devs::Atomic<Message> {
public:
    double time_advance() const override {
        return sent_ < 3 ? 1 : std::numeric_limits<double>::infinity();
    }
    std::vector<PortValue> output() const override {
        const double time = sent_ + 1;
        return {{0, Message{Entity{static_cast<std::uint64_t>(sent_ + 1), time, 2}}}};
    }
    void internal_transition() override { ++sent_; }
    void external_transition(double, const std::vector<Input>&) override { throw std::logic_error("source input"); }
    void confluent_transition(const std::vector<Input>&) override { throw std::logic_error("source confluence"); }
private:
    int sent_ = 0;
};

void test_des_completion_drives_sd_stock() {
    ankurafathom::devs::Simulator<Message> simulator;
    const auto source = simulator.add(std::make_unique<Source>());
    const auto server = simulator.add(std::make_unique<ankurafathom::des::SingleServer<Message>>());
    const auto bridge = simulator.add(std::make_unique<ankurafathom::hybrid::EntityToPulse<Message>>(
        [](const Entity& entity) { return static_cast<double>(entity.id); }));
    ankurafathom::sd::Model stocks;
    const auto stock = stocks.add_stock("completed_value", 0);
    auto clocked = std::make_unique<ankurafathom::hybrid::ClockedSD<Message>>(std::move(stocks), 1);
    auto* observed = clocked.get();
    const auto sd = simulator.add(std::move(clocked));
    simulator.connect(source, 0, server, 0);
    simulator.connect(server, 1, bridge, 0);
    simulator.connect(bridge, 1, sd, static_cast<std::uint32_t>(stock));
    const auto trace = simulator.run_until(7);
    std::vector<double> pulse_times;
    for (const auto& step : trace) {
        for (const auto& emission : step.emissions) {
            if (emission.source == bridge) pulse_times.push_back(step.time);
        }
    }
    require(pulse_times == std::vector<double>({3, 5, 7}),
            "DES completions must become same-time SD pulses");
    require(observed->model().state()[stock] == 6,
            "SD stock must equal the sum of completed entity values");
}

} // namespace

int main() {
    try {
        test_des_completion_drives_sd_stock();
        std::cout << "DES-to-SD hybrid test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
