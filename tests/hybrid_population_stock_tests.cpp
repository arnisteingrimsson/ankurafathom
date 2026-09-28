#include "ankurafathom/hybrid/clocked_sd.hpp"
#include "ankurafathom/hybrid/population_to_stock.hpp"
#include "ankurafathom/des/single_server.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <variant>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_committed_population_aggregate() {
    ankurafathom::abm::SyncPopulation<double> population;
    const auto first = population.spawn(1);
    const auto second = population.spawn(2);
    population.add_phase([](std::size_t index, const auto& snapshot) {
        return snapshot[index].value + 1;
    });

    ankurafathom::sd::Model stocks;
    const auto stock = stocks.add_stock("active_total", 0);
    auto clocked = std::make_unique<ankurafathom::hybrid::ClockedSD<double>>(
        std::move(stocks), 1);
    auto* stock_ptr = clocked.get();
    auto aggregate = std::make_unique<ankurafathom::hybrid::PopulationToStock<double>>(
        population, [](double value) { return value; });
    auto* aggregate_ptr = aggregate.get();
    ankurafathom::devs::Simulator<double> simulator;
    const auto sd_id = simulator.add(std::move(clocked));
    const auto aggregate_id = simulator.add(std::move(aggregate));
    simulator.connect(aggregate_id, 1, sd_id, static_cast<std::uint32_t>(stock));

    const std::vector<double> expected_totals{3, 5, 9, 6, 7, 0};
    const std::vector<double> expected_deltas{3, 2, 4, -3, 1, -7};
    for (std::size_t step = 0; step < expected_totals.size(); ++step) {
        const double time = static_cast<double>(step);
        (void)simulator.run_until(time);
        if (step == 1) population.step();
        if (step == 2) {
            population.despawn(first);
            require(population.spawn(4) == 2, "agent IDs must remain monotonic");
            population.step();
        }
        if (step == 3) {
            population.despawn(second);
            population.step();
        }
        if (step == 4) population.step();
        if (step == 5) population.despawn(2);
        simulator.inject(time, aggregate_id, 0, 0);
        const auto trace = simulator.run_until(time);
        std::vector<double> deltas;
        for (const auto& event : trace)
            for (const auto& emission : event.emissions)
                if (emission.source == aggregate_id) {
                    require(event.time == time, "agent aggregate pulse moved off its tick");
                    deltas.push_back(emission.value);
                }
        require(deltas == std::vector<double>{expected_deltas[step]},
                "aggregate bridge emitted the wrong delta");
        double direct_total = 0;
        for (const auto& agent : population.records())
            if (agent.alive) direct_total += agent.value;
        require(stock_ptr->model().state()[stock] == direct_total &&
                direct_total == expected_totals[step] &&
                aggregate_ptr->published_total() == direct_total,
                "SD stock does not equal committed active-agent aggregate");
        if (step == 3) {
            simulator.inject(time, aggregate_id, 0, 0);
            const auto unchanged = simulator.run_until(time);
            for (const auto& event : unchanged)
                for (const auto& emission : event.emissions)
                    require(emission.source != aggregate_id,
                            "unchanged population must not emit another aggregate pulse");
        }
    }
}

void test_invalid_contribution_rolls_back() {
    ankurafathom::abm::SyncPopulation<double> population;
    (void)population.spawn(1);
    bool invalid = true;
    ankurafathom::hybrid::PopulationToStock<double> bridge(population,
        [&invalid](double value) {
            return invalid ? std::numeric_limits<double>::quiet_NaN() : value;
        });
    const std::vector<ankurafathom::devs::Input<double>> trigger{{0, 0, 0}};
    bool caught = false;
    try { bridge.external_transition(0, trigger); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught && bridge.published_total() == 0 && !std::isfinite(bridge.time_advance()),
            "invalid aggregate must leave bridge state unchanged");
    invalid = false;
    bridge.external_transition(0, trigger);
    require(bridge.output().size() == 1 && bridge.output()[0].value == 1 &&
            bridge.published_total() == 1,
            "valid retry failed after invalid contribution");
}

void test_typed_hybrid_payload() {
    using Message = std::variant<ankurafathom::des::Entity, double>;
    ankurafathom::abm::SyncPopulation<bool> population;
    (void)population.spawn(true);
    (void)population.spawn(false);
    ankurafathom::sd::Model stocks;
    const auto stock = stocks.add_stock("adopted", 0);
    auto clocked = std::make_unique<ankurafathom::hybrid::ClockedSD<Message>>(
        std::move(stocks), 1);
    auto* observed = clocked.get();
    ankurafathom::devs::Simulator<Message> simulator;
    const auto sd_id = simulator.add(std::move(clocked));
    const auto aggregate_id = simulator.add(
        std::make_unique<ankurafathom::hybrid::PopulationToStock<bool, Message>>(
            population, [](bool adopted) { return adopted ? 1.0 : 0.0; }));
    simulator.connect(aggregate_id, 1, sd_id, static_cast<std::uint32_t>(stock));
    simulator.inject(0, aggregate_id, 0, Message{0.0});
    (void)simulator.run_until(0);
    require(observed->model().state()[stock] == 1,
            "typed ABM-to-SD aggregate lost its numeric pulse");
}

} // namespace

int main() {
    try {
        test_committed_population_aggregate();
        test_invalid_contribution_rolls_back();
        test_typed_hybrid_payload();
        std::cout << "population-to-stock hybrid tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
