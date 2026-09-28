#include "ankurafathom/ir/model.hpp"
#include "ankurafathom/hybrid/agent_pool_completion_to_pulse.hpp"
#include "ankurafathom/hybrid/agent_pool_process_atomic.hpp"
#include "ankurafathom/hybrid/clocked_sd.hpp"

#include <algorithm>
#include <array>
#include <iostream>

namespace {
using namespace ankurafathom;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
double value(const std::vector<ir::Row>& rows, double time, const std::string& id) {
    for (const auto& row : rows) if (row.time == time && row.output_id == id) return row.value;
    throw std::runtime_error("missing output " + id);
}
void test_hand_oracle_and_reference(const ir::Model& model) {
    const auto rows = ir::run(model);
    const std::array<double, 9> completed{0, 0, 1, 1, 3, 3, 4, 5, 5};
    const std::array<double, 9> exposure{0, 0, 0, 50, 100, 250, 400, 625, 875};
    require(rows.size() == 9 * model.agent_pool->outputs.size(), "wrong hybrid row count");
    for (std::size_t step = 0; step < 9; ++step) {
        const double time = step * 0.5;
        require(value(rows, time, "revenue") == 100 * completed[step] &&
                value(rows, time, "completed") == completed[step] &&
                value(rows, time, "cost") == time * 20 &&
                value(rows, time, "revenue_time") == exposure[step],
                "coupled model differs from exact event-time hand oracle");
        for (std::size_t j = 0; j < model.agent_pool->outputs.size(); ++j)
            require(rows[step * model.agent_pool->outputs.size() + j].output_id ==
                    model.agent_pool->outputs[j].id, "mixed outputs changed declaration order");
    }
    require(value(rows, 4, "utilization") == 23.0 / 26.0 &&
            value(rows, 4, "wait_total") == 4.5 && value(rows, 4, "cycle_total") == 9.25,
            "coupling changed delivery accounting");
    auto standalone = model;
    standalone.kind = ir::Model::Kind::agent_pool;
    std::erase_if(standalone.agent_pool->outputs, [](const auto& output) {
        return output.source == ir::AgentPoolOutput::Source::stock;
    });
    for (const auto& row : ir::run(standalone))
        require(row.value == value(rows, row.time, row.output_id),
                "DEVS staffing observations differ from standalone reference");
    const auto replay = ir::run(model);
    const auto overridden = ir::run(model, {{"daily_cost", 35}});
    for (std::size_t i = 0; i < rows.size(); ++i) {
        require(rows[i].time == replay[i].time && rows[i].output_id == replay[i].output_id &&
                rows[i].value == replay[i].value, "coupled run does not replay");
        const double expected = rows[i].output_id == "cost" ? rows[i].time * 35 : rows[i].value;
        require(overridden[i].value == expected, "scenario override did not isolate the SD parameter");
    }
    auto refined = model;
    refined.dt = 0.25;
    const auto fine = ir::run(refined);
    for (const auto& row : rows)
        require(value(fine, row.time, row.output_id) == row.value,
                "refinement changed exact constant-flow or delivery oracle");
    refined.horizon = 3.25;
    const auto ending = ir::run(refined);
    require(value(ending, 3.25, "revenue") == 500 && value(ending, 3.25, "revenue_time") == 500 &&
            value(ending, 3.25, "completed") == 5, "horizon omitted pending publication microsteps");
    auto short_run = model;
    short_run.horizon = 2;
    short_run.agent_pool->schedule.pop_back();
    const auto unfinished = ir::run(short_run);
    require(value(unfinished, 2, "revenue") == 300 && value(unfinished, 2, "in_service") == 1 &&
            value(unfinished, 2, "waiting") == 1, "unfinished work contributed completion revenue");
}

void test_boundaries_and_failures(const ir::Model& model) {
    auto idle = model;
    idle.agent_pool->schedule.clear();
    const auto empty = ir::run(idle);
    require(value(empty, 4, "revenue") == 0 && value(empty, 4, "revenue_time") == 0 &&
            value(empty, 4, "cost") == 80, "idle delivery created pulses or stopped SD flows");
    auto tiny = idle;
    ir::AgentPoolChange change{};
    change.engagements = {{14, 1, 1e-18}};
    tiny.agent_pool->schedule = {change};
    const auto early = ir::run(tiny);
    require(value(early, 0, "revenue") == 0 && value(early, 0.5, "revenue") == 100,
            "positive duration completed at time zero");
    auto near = idle;
    change.engagements = {{14, 1, std::nextafter(0.5, 1.0)}};
    near.agent_pool->schedule = {change};
    const auto distinct = ir::run(near);
    require(value(distinct, 0.5, "revenue") == 0 && value(distinct, 1, "revenue") == 100,
            "observation rounded a future completion backward");

    auto failed = model;
    failed.agent_pool->phases[0].capacity_expression = ir::Expression("capacity / (active - active)");
    bool rejected = false;
    try { (void)ir::run(failed); }
    catch (const ir::Error& error) {
        rejected = error.code == "IR_AGENT_POOL_PHASE" &&
                   error.pointer == "/agent_pool/schedule/3/phases/0";
    }
    require(rejected, "coupled runtime lost phase diagnostics");
    failed = model;
    failed.completion_bridge->amount = std::numeric_limits<double>::max();
    rejected = false;
    try { (void)ir::run(failed); }
    catch (const ir::Error& error) { rejected = error.code == "IR_AGENT_POOL_SD_RUNTIME"; }
    require(rejected, "nonfinite coupled stock was accepted");
}

void test_bridge_and_exact_sd_clock() {
    using Process = hybrid::AgentPoolProcess<std::size_t>;
    using Message = std::variant<Process::Change, Process::Result, double>;
    using Bridge = hybrid::AgentPoolCompletionToPulse<std::size_t, Message>;
    Bridge bridge(100);
    Process::Result completed;
    completed.completed = {1, 2};
    bridge.external_transition(0, {{0, Bridge::input_port, completed}});
    require(std::get<double>(bridge.output().at(0).value) == 200, "batch must pulse per engagement");
    bool rejected = false;
    try { bridge.confluent_transition({{0, Bridge::input_port, completed}, {0, 9, completed}}); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected && std::get<double>(bridge.output().at(0).value) == 200,
            "invalid confluent bag destroyed pending pulse");
    Process::Result staffing_only;
    bridge.confluent_transition({{0, Bridge::input_port, staffing_only}});
    require(bridge.output().empty() && std::isinf(bridge.time_advance()),
            "staffing-only result must not create an SD event");

    constexpr double intermediate = 8.943065046370474;
    constexpr double deadline = 50.058611305029835;
    sd::Model stocks;
    (void)stocks.add_stock("total", 0);
    using SD = hybrid::ClockedSD<double>;
    devs::Simulator<double> simulator;
    const auto id = simulator.add(std::make_unique<SD>(std::move(stocks), deadline));
    simulator.inject(intermediate, id, 0, 1);
    (void)simulator.step_transactional();
    require(simulator.next_time() == deadline, "SD tick drifted after off-grid pulse");
    simulator.inject(deadline, id, 0, 2);
    (void)simulator.run_until_transactional(deadline);
    const auto& result = dynamic_cast<const SD&>(simulator.model(id));
    require(result.time() == deadline && result.model().state()[0] == 3,
            "SD exact clock did not preserve tick/pulse confluence");
    // A passive interval before its first tick exercises absolute external time.
    sd::Model other;
    (void)other.add_stock("total", 0);
    devs::Simulator<double> external;
    const auto other_id = external.add(std::make_unique<SD>(std::move(other), 100));
    external.inject(intermediate, other_id, 0, 1);
    external.inject(deadline, other_id, 0, 2);
    (void)external.run_until_transactional(deadline);
    require(dynamic_cast<const SD&>(external.model(other_id)).time() == deadline,
            "SD reconstructed external time from a rounded delay");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected agent-pool SD fixture");
        const auto model = ir::load_file(argv[1]);
        require(model.kind == ir::Model::Kind::agent_pool_sd, "wrong mode");
        test_hand_oracle_and_reference(model);
        test_boundaries_and_failures(model);
        test_bridge_and_exact_sd_clock();
        std::cout << "agent-pool SD IR tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
