#include "ankurafathom/runtime/experiment.hpp"
#include "ankurafathom/rng/philox.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void rejects(Function&& function, const char* message) {
    bool caught = false;
    try { function(); } catch (const std::invalid_argument&) { caught = true; }
    require(caught, message);
}

void test_order_address_and_statistics() {
    using namespace ankurafathom;
    const runtime::Experiment experiment{1234, {{0, {{"demand", 2}}}, {3, {{"demand", 4}}}}, 4};
    const auto execute = [](const runtime::Scenario& scenario, std::uint32_t replication, std::uint64_t seed) {
        rng::DrawAddress address{scenario.id, replication, 7, 2, 1, 0};
        const double draw = rng::uniform_open(rng::draw(seed, address)[0]);
        return std::vector<runtime::Observation>{{0, "revenue", scenario.parameters.at("demand") + draw}};
    };
    const auto first = runtime::run_experiment(experiment, execute);
    const auto replay = runtime::run_experiment(experiment, execute);
    require(first.size() == 8 && replay.size() == first.size(), "incorrect trajectory count");
    for (std::size_t i = 0; i < first.size(); ++i) {
        require(first[i].scenario == replay[i].scenario &&
                first[i].replication == replay[i].replication &&
                first[i].observations[0].value == replay[i].observations[0].value,
                "same seed and experiment must replay exactly");
        require(first[i].scenario == (i < 4 ? 0U : 3U) && first[i].replication == i % 4,
                "trajectory order must be scenario then replication");
    }
    const auto statistics = runtime::summarize(first);
    require(statistics.size() == 2 && statistics[0].count == 4 && statistics[1].count == 4,
            "ensemble must aggregate each scenario independently");
    for (std::size_t group = 0; group < 2; ++group) {
        double mean = 0;
        for (std::size_t i = group * 4; i < group * 4 + 4; ++i)
            mean += first[i].observations[0].value / 4;
        double variance = 0;
        for (std::size_t i = group * 4; i < group * 4 + 4; ++i) {
            const double delta = first[i].observations[0].value - mean;
            variance += delta * delta / 3;
        }
        require(std::abs(statistics[group].mean - mean) < 1e-14 &&
                std::abs(statistics[group].sample_variance - variance) < 1e-14,
                "sample statistics are incorrect");
    }
}

void test_invalid_inputs() {
    using namespace ankurafathom::runtime;
    rejects([] { validate(Experiment{0, {{0, {}}}, 0}); }, "zero replications must fail");
    rejects([] { validate(Experiment{0, {{0, {}}}, 65537}); }, "replication overflow must fail");
    rejects([] { validate(Experiment{0, {{1, {}}, {1, {}}}, 1}); }, "duplicate IDs must fail");
    rejects([] { validate(Experiment{0, {{65536, {}}}, 1}); }, "scenario ID overflow must fail");
    rejects([] {
        (void)run_experiment(Experiment{0, {{0, {}}}, 1},
            [](const Scenario&, std::uint32_t, std::uint64_t) {
                return std::vector<Observation>{{1, "x", 1}, {1, "x", 2}};
            });
    }, "duplicate observations must fail");
}

} // namespace

int main() {
    try {
        test_order_address_and_statistics();
        test_invalid_inputs();
        std::cout << "Experiment tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
