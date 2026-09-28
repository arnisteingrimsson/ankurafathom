#include "ankurafathom/ir/model.hpp"
#include "ankurafathom/hybrid/transactional_agent_pool.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

double at(const std::vector<ankurafathom::ir::Row>& rows,
          std::size_t step, std::size_t output_count, const std::string& id) {
    for (std::size_t i = 0; i < output_count; ++i)
        if (rows[step * output_count + i].output_id == id)
            return rows[step * output_count + i].value;
    throw std::runtime_error("missing output: " + id);
}

void test_reference(const ankurafathom::ir::Model& model,
                    const std::vector<ankurafathom::ir::Row>& rows) {
    const auto& spec = *model.agent_pool;
    ankurafathom::abm::SyncPopulation<std::size_t> population;
    for (auto capacity : spec.initial_capacities) (void)population.spawn(capacity);
    ankurafathom::hybrid::TransactionalAgentPool<std::size_t> reference(
        std::move(population), [](const std::size_t& value) { return value; },
        spec.max_request_units);
    std::set<std::uint64_t> granted;
    std::size_t next = 0;
    for (std::size_t step = 0; step <= 4; ++step) {
        while (next < spec.schedule.size() && spec.schedule[next].time <= step) {
            const auto& item = spec.schedule[next++];
            ankurafathom::hybrid::TransactionalAgentPool<std::size_t>::Change change;
            change.time = item.time;
            change.releases = item.releases;
            change.updates = item.updates;
            change.departures = item.departures;
            change.hires = item.hires;
            change.requests = item.requests;
            const auto result = reference.apply(change);
            for (const auto& grant : result.grants) granted.insert(grant.request_id);
        }
        const auto count = spec.outputs.size();
        require(at(rows, step, count, "capacity") == reference.pool().capacity(),
                "capacity differs from transaction reference");
        require(at(rows, step, count, "available") == reference.pool().available(),
                "available differs from transaction reference");
        require(at(rows, step, count, "allocated") == reference.pool().allocated_units(),
                "allocation differs from transaction reference");
        require(at(rows, step, count, "waiting") == reference.pool().waiting(),
                "queue differs from transaction reference");
        require(at(rows, step, count, "active") == reference.population().active_count(),
                "population differs from transaction reference");
        require(at(rows, step, count, "new_agent_allocated") ==
                    reference.broker().allocated_to(2),
                "agent assignment differs from transaction reference");
        require(at(rows, step, count, "third_granted") ==
                    (granted.contains(3) ? 1.0 : 0.0),
                "grant history differs from transaction reference");
    }
}

void test_hand_oracle(const ankurafathom::ir::Model& model,
                      const std::vector<ankurafathom::ir::Row>& rows) {
    const auto count = model.agent_pool->outputs.size();
    require(rows.size() == 5 * count, "wrong observation count");
    const std::array<double, 5> capacity{2, 1, 2, 1, 0};
    const std::array<double, 5> allocated{2, 1, 2, 1, 0};
    const std::array<double, 5> waiting{1, 1, 0, 0, 0};
    const std::array<double, 5> active{2, 1, 2, 2, 1};
    const std::array<double, 5> new_agent{0, 0, 1, 1, 0};
    const std::array<double, 5> granted{0, 0, 1, 1, 1};
    for (std::size_t step = 0; step < 5; ++step) {
        require(rows[step * count].time == step, "wrong observation time");
        require(at(rows, step, count, "capacity") == capacity[step], "wrong capacity");
        require(at(rows, step, count, "available") == 0, "wrong available units");
        require(at(rows, step, count, "allocated") == allocated[step], "wrong allocation");
        require(at(rows, step, count, "waiting") == waiting[step], "wrong waiting count");
        require(at(rows, step, count, "active") == active[step], "wrong active count");
        require(at(rows, step, count, "new_agent_allocated") == new_agent[step],
                "wrong agent assignment");
        require(at(rows, step, count, "third_granted") == granted[step],
                "wrong cumulative grant state");
    }
}

void test_runtime_failure(const ankurafathom::ir::Model& model) {
    auto invalid = model;
    invalid.agent_pool->schedule[1].releases.clear();
    bool caught = false;
    try { (void)ankurafathom::ir::run(invalid); }
    catch (const ankurafathom::ir::Error& error) {
        caught = error.code == "IR_AGENT_POOL_RUNTIME" &&
                 error.pointer == "/agent_pool/schedule/1";
    }
    require(caught, "unsafe departure must report the scheduled transaction");
    require(ankurafathom::ir::run(model).size() == 5 * model.agent_pool->outputs.size(),
            "failed run must not change the declarative model");
}

void test_schedule_sampling(const ankurafathom::ir::Model& model) {
    auto off_grid = model;
    off_grid.agent_pool->schedule[2].time = 1.5;
    const auto off_grid_rows = ankurafathom::ir::run(off_grid);
    const auto baseline = ankurafathom::ir::run(model);
    require(off_grid_rows.size() == baseline.size() &&
            std::equal(off_grid_rows.begin(), off_grid_rows.end(), baseline.begin(),
                [](const auto& left, const auto& right) {
                    return left.time == right.time && left.output_id == right.output_id &&
                           left.value == right.value;
                }),
            "off-grid hire should first appear at the next observation");

    auto same_time = model;
    same_time.agent_pool->schedule[2].time = 1;
    const auto rows = ankurafathom::ir::run(same_time);
    const auto count = same_time.agent_pool->outputs.size();
    require(at(rows, 1, count, "capacity") == 2 &&
            at(rows, 1, count, "third_granted") == 1 &&
            at(rows, 1, count, "waiting") == 0,
            "same-time changes must apply in schedule order before sampling");

    auto fractional = model;
    fractional.dt = 0.7;
    fractional.horizon = 2.8;
    for (std::size_t i = 0; i < fractional.agent_pool->schedule.size(); ++i)
        fractional.agent_pool->schedule[i].time = 0.7 * static_cast<double>(i);
    fractional.agent_pool->schedule[3].time = 2.1;
    const auto fractional_rows = ankurafathom::ir::run(fractional);
    require(at(fractional_rows, 3, count, "capacity") == 1 &&
            at(fractional_rows, 4, count, "capacity") == 0,
            "decimal grid ties must apply at their intended tick");
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected agent-pool IR path");
        const auto model = ankurafathom::ir::load_file(argv[1]);
        require(model.kind == ankurafathom::ir::Model::Kind::agent_pool,
                "wrong IR mode");
        const auto rows = ankurafathom::ir::run(model);
        test_reference(model, rows);
        test_hand_oracle(model, rows);
        test_runtime_failure(model);
        test_schedule_sampling(model);
        std::cout << "agent-pool IR tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
