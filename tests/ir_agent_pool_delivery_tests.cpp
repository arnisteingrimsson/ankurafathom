#include "ankurafathom/ir/model.hpp"

#include <array>
#include <iostream>

namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void test_delivery(const ankurafathom::ir::Model& model) {
    const auto rows = ankurafathom::ir::run(model);
    // capacity, allocated, accepted, started, completed, in-service, waiting,
    // wait total, completed cycle total, final engagement running/completed/granted.
    const std::array<std::array<double, 12>, 9> expected{{
        {{2, 2, 4, 2, 0, 2, 2, 0, 0, 0, 0, 0}},
        {{2, 2, 4, 2, 0, 2, 2, 0, 0, 0, 0, 0}},
        {{1, 1, 4, 2, 1, 1, 2, 0, 1, 0, 0, 0}},
        {{2, 2, 4, 3, 1, 2, 1, 1.5, 1, 0, 0, 0}},
        {{2, 2, 5, 4, 3, 1, 1, 3.5, 5, 0, 0, 0}},
        {{2, 2, 5, 4, 3, 1, 1, 3.5, 5, 0, 0, 0}},
        {{1, 1, 5, 5, 4, 1, 0, 4.5, 8, 1, 0, 1}},
        {{1, 0, 5, 5, 5, 0, 0, 4.5, 9.25, 0, 1, 1}},
        {{1, 0, 5, 5, 5, 0, 0, 4.5, 9.25, 0, 1, 1}}
    }};
    require(rows.size() == 108, "wrong delivery observation count");
    for (std::size_t step = 0; step < expected.size(); ++step)
        for (std::size_t metric = 0; metric < expected[step].size(); ++metric) {
            const auto& row = rows.at(step * 12 + metric);
            require(row.time == step * 0.5 && row.value == expected[step][metric],
                    "delivery trajectory differs from exact hand oracle");
        }

    auto short_run = model;
    short_run.horizon = 2;
    short_run.agent_pool->schedule.pop_back();
    const auto short_rows = ankurafathom::ir::run(short_run);
    require(short_rows.size() == 60 && short_rows[52].value == 3 && short_rows[53].value == 1 &&
            short_rows[54].value == 1, "horizon must preserve unfinished and queued engagements");

    auto failed = model;
    failed.agent_pool->schedule[1].time = 0.5;
    bool rejected = false;
    try { (void)ankurafathom::ir::run(failed); }
    catch (const ankurafathom::ir::Error& error) {
        rejected = error.code == "IR_AGENT_POOL_RUNTIME" && error.pointer == "/agent_pool/schedule/1";
    }
    require(rejected, "departure before delivery completion must fail at its transaction");

    auto tiny = model;
    ankurafathom::ir::AgentPoolChange change{};
    change.engagements = {{14, 1, 1e-18}};
    tiny.agent_pool->schedule = {change};
    const auto tiny_rows = ankurafathom::ir::run(tiny);
    require(tiny_rows[4].value == 0 && tiny_rows[9].value == 1 && tiny_rows[16].value == 1,
            "positive delivery duration must never be observed complete at time zero");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected delivery fixture path");
        test_delivery(ankurafathom::ir::load_file(argv[1]));
        std::cout << "agent-pool delivery IR tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
