#include "ankurafathom/abm/sync_population.hpp"
#include "ankurafathom/rng/philox.hpp"
#include "ankurafathom/sd/model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_abm_mean_matches_sd_bass() {
    constexpr int agents = 2000;
    constexpr int replications = 24;
    constexpr int steps = 80;
    constexpr double dt = 0.1;
    constexpr double innovation = 0.03;
    constexpr double imitation = 0.7;

    ankurafathom::sd::Model mean_field;
    const auto adoption = mean_field.add_stock("adoption_fraction", 0);
    mean_field.add_flow(ankurafathom::sd::Model::boundary, adoption,
        [adoption](const auto& state, double) {
            const double fraction = state[adoption];
            return (innovation + imitation * fraction) * (1 - fraction);
        });

    std::vector<double> average(static_cast<std::size_t>(steps + 1), 0);
    for (int replication = 0; replication < replications; ++replication) {
        ankurafathom::abm::SyncPopulation<bool> population;
        for (int i = 0; i < agents; ++i) population.spawn(false);
        int step_number = 0;
        double prior_fraction = 0;
        population.add_phase([&](std::size_t index, const auto& snapshot) {
            if (snapshot[index].value) return true;
            const double probability = (innovation + imitation * prior_fraction) * dt;
            const ankurafathom::rng::DrawAddress address{
                0, static_cast<std::uint32_t>(replication), snapshot[index].id,
                static_cast<std::uint32_t>(step_number), 0, 0};
            return ankurafathom::rng::bernoulli(probability,
                                               ankurafathom::rng::draw(0x12345678ULL, address)[0]);
        });
        for (int step = 1; step <= steps; ++step) {
            step_number = step;
            int prior_adopted = 0;
            for (const auto& agent : population.records()) if (agent.value) ++prior_adopted;
            prior_fraction = static_cast<double>(prior_adopted) / agents;
            population.step();
            int adopted = 0;
            for (const auto& agent : population.records()) if (agent.value) ++adopted;
            average[static_cast<std::size_t>(step)] +=
                static_cast<double>(adopted) / (agents * replications);
        }
    }

    double max_gap = 0;
    for (int step = 1; step <= steps; ++step) {
        mean_field.step((step - 1) * dt, dt);
        max_gap = std::max(max_gap, std::abs(average[static_cast<std::size_t>(step)] -
                                             mean_field.state()[adoption]));
    }
    require(max_gap < 0.025, "ABM adoption ensemble diverges from the SD mean field");
}

} // namespace

int main() {
    try {
        test_abm_mean_matches_sd_bass();
        std::cout << "hybrid Bass mean-field test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
