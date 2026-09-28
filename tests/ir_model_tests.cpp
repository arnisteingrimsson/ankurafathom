#include "ankurafathom/ir/model.hpp"
#include "ankurafathom/des/process.hpp"
#include "ankurafathom/des/multi_server.hpp"
#include "ankurafathom/hybrid/clocked_sd.hpp"
#include "ankurafathom/hybrid/entity_to_pulse.hpp"
#include "ankurafathom/hybrid/stock_to_entity.hpp"
#include "ankurafathom/hybrid/population_to_stock.hpp"
#include "ankurafathom/abm/sync_population.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_expression() {
    const ankurafathom::ir::Expression expression("2 + 3 * (stock - 1) / 2");
    require(expression.evaluate(std::map<std::string, double>{{"stock", 5}}) == 8,
            "expression precedence or evaluation is wrong");
    bool caught = false;
    try { (void)ankurafathom::ir::Expression("2 + * 3"); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught, "malformed expression must be rejected");
}

void test_loader_run_and_diagnostic(const std::string& valid_path, const std::string& invalid_path,
                                    const std::string& invalid_unit_path,
                                    const std::string& invalid_field_path) {
    const auto model = ankurafathom::ir::load_file(valid_path);
    const auto rows = ankurafathom::ir::run(model);
    require(rows.size() == 11, "IR decay fixture should produce initial state plus ten steps");
    require(rows.front().time == 0 && rows.front().value == 100,
            "IR initial output is wrong");
    require(std::abs(rows.back().value - 100 * std::pow(0.98, 10)) < 1e-10,
            "IR-run Euler decay does not match analytic recurrence");
    const auto overridden = ankurafathom::ir::run(model, {{"decay_rate", 0.1}});
    require(std::abs(overridden.back().value - 100 * std::pow(0.99, 10)) < 1e-10,
            "scenario override did not change the declared parameter");
    bool caught = false;
    try { (void)ankurafathom::ir::run(model, {{"unknown", 0.1}}); }
    catch (const ankurafathom::ir::Error& error) { caught = error.code == "IR_OVERRIDE"; }
    require(caught, "unknown override should have a structured diagnostic");
    try { (void)ankurafathom::ir::load_file(invalid_path); }
    catch (const ankurafathom::ir::Error& error) {
        caught = error.code == "IR_SYMBOL" && error.pointer == "/components/1/expr";
    }
    require(caught, "invalid IR must report stable symbol code and JSON pointer");
    caught = false;
    try { (void)ankurafathom::ir::load_file(invalid_unit_path); }
    catch (const ankurafathom::ir::Error& error) {
        caught = error.code == "IR_UNIT" && error.pointer == "/components/1/unit";
    }
    require(caught, "dimensionally invalid flow must be rejected");
    caught = false;
    try { (void)ankurafathom::ir::load_file(invalid_field_path); }
    catch (const ankurafathom::ir::Error& error) {
        caught = error.code == "IR_FIELD" && error.pointer == "/components/0/non_negativ";
    }
    require(caught, "unknown field must be rejected with a JSON pointer");
}

void test_lookup_model(const std::string& valid_path, const std::string& invalid_unit_path) {
    const auto model = ankurafathom::ir::load_file(valid_path);
    const auto rows = ankurafathom::ir::run(model);
    require(rows.size() == 3 && rows[0].value == 100 && rows[1].value == 99 &&
            rows[2].value == 97, "table-driven flow produced the wrong Euler trajectory");
    bool caught = false;
    try { (void)ankurafathom::ir::load_file(invalid_unit_path); }
    catch (const ankurafathom::ir::Error& error) {
        caught = error.code == "IR_UNIT" && error.pointer == "/components/2/expr";
    }
    require(caught, "lookup argument units must be checked");
}

void test_delay_models(const std::string& delay_path, const std::string& chain_path,
                       const std::string& invalid_unit_path) {
    const auto model = ankurafathom::ir::load_file(delay_path);
    const auto delayed = ankurafathom::ir::run(model);
    require(delayed.size() == 10 && delayed[0].value == 0 && delayed[1].value == 0 &&
            delayed[3].value == 5 && delayed[4].value == 5 &&
            delayed[8].value == 21.25 && delayed[9].value == 9.375,
            "material delay IR trajectory is wrong");
    const auto doubled = ankurafathom::ir::run(model, {{"intake", 20}});
    require(doubled[8].value == 42.5 && doubled[9].value == 18.75,
            "scenario override did not reach the material delay input");
    const auto chained = ankurafathom::ir::run(ankurafathom::ir::load_file(chain_path));
    require(chained.size() == 12 && chained[3].value == 0 && chained[4].value == 10 &&
            chained[5].value == 0 && chained[6].value == 0 && chained[7].value == 10 &&
            chained[8].value == 10 && chained[9].value == 10,
            "delay chain must use one shared pre-step state");
    bool caught = false;
    try { (void)ankurafathom::ir::load_file(invalid_unit_path); }
    catch (const ankurafathom::ir::Error& error) {
        caught = error.code == "IR_UNIT" && error.pointer == "/components/1/input";
    }
    require(caught, "delay input dimensions must be checked");
    caught = false;
    try { (void)ankurafathom::ir::run(model, {{"intake", -1}}); }
    catch (const ankurafathom::ir::Error& error) {
        caught = error.code == "IR_DELAY_RUNTIME" && error.pointer == "/components/1/input";
    }
    require(caught, "invalid material input needs a component-specific runtime diagnostic");
}

void test_des_process(const std::string& process_path, const std::string& invalid_link_path) {
    const auto model = ankurafathom::ir::load_file(process_path);
    require(model.kind == ankurafathom::ir::Model::Kind::des,
            "process model did not select the DES runtime");
    const auto rows = ankurafathom::ir::run(model);
    require(rows.size() == 36, "DES process should emit nine metrics at four grid times");
    const auto value = [&rows](double time, const std::string& id) {
        for (const auto& row : rows)
            if (row.time == time && row.output_id == id) return row.value;
        throw std::runtime_error("missing DES process output");
    };
    require(value(0, "emitted") == 3 && value(0, "accepted") == 3 &&
            value(0, "completed") == 0 && value(0, "waiting") == 1 &&
            value(0, "busy") == 2 && value(0, "cycle_mean") == 0,
            "initial same-time process bag is wrong");
    require(value(1, "emitted") == 4 && value(1, "completed") == 1 &&
            value(1, "waiting") == 1 && value(2, "completed") == 3 &&
            value(2, "waiting") == 0,
            "process confluence or FIFO queue schedule is wrong");
    require(value(3, "completed") == 4 && value(3, "busy") == 0 &&
            value(3, "cycle_total") == 7 && value(3, "cycle_mean") == 1.75 &&
            std::abs(value(3, "queue_mean") - 2.0 / 3.0) < 1e-12 &&
            std::abs(value(3, "utilization") - 5.0 / 6.0) < 1e-12,
            "process end-state statistics differ from the exact queue oracle");
    bool caught = false;
    try { (void)ankurafathom::ir::load_file(invalid_link_path); }
    catch (const ankurafathom::ir::Error& error) {
        caught = error.code == "IR_LINK" && error.pointer == "/links/0";
    }
    require(caught, "invalid process topology needs a structured link diagnostic");
}

void test_stochastic_des_process(const std::string& path) {
    const auto model = ankurafathom::ir::load_file(path);
    const auto first = ankurafathom::ir::run(model, {}, 987, 2, 0);
    const auto replay = ankurafathom::ir::run(model, {}, 987, 2, 0);
    const auto second = ankurafathom::ir::run(model, {}, 987, 2, 1);
    require(first.size() == 183 && first.size() == replay.size() &&
            first.size() == second.size(), "stochastic DES observation grid is wrong");
    bool differs = false;
    for (std::size_t i = 0; i < first.size(); ++i) {
        require(first[i].time == replay[i].time && first[i].output_id == replay[i].output_id &&
                first[i].value == replay[i].value,
                "same seed/scenario/replication must replay bit-for-bit");
        differs |= first[i].value != second[i].value;
    }
    require(differs, "replication address did not alter the stochastic process");
    require(first[first.size() - 3].value == 30 && first[first.size() - 2].value == 30 &&
            first.back().value > 0 && second[second.size() - 2].value == 30,
            "stochastic source or sink lost completed entities by the horizon");
}

void test_tandem_des_process(const std::string& path, const std::string& invalid_branch_path) {
    const auto rows = ankurafathom::ir::run(ankurafathom::ir::load_file(path));
    require(rows.size() == 54, "two-stage process output grid is wrong");
    const auto value = [&rows](double time, const std::string& id) {
        for (const auto& row : rows)
            if (row.time == time && row.output_id == id) return row.value;
        throw std::runtime_error("missing two-stage process output");
    };
    require(value(0, "created") == 2 && value(1, "review_done") == 1 &&
            value(1, "delivery_busy") == 1 && value(2, "review_done") == 2 &&
            value(2, "delivery_wait") == 1 && value(2, "completed") == 0,
            "upstream completion or downstream admission is wrong");
    require(value(3, "completed") == 1 && value(3, "delivery_wait") == 0 &&
            value(5, "completed") == 2 && value(5, "cycle_total") == 8 &&
            value(5, "cycle_mean") == 4 &&
            std::abs(value(5, "delivery_util") - 0.8) < 1e-12 &&
            std::abs(value(5, "delivery_queue_mean") - 0.2) < 1e-12,
            "two-stage IR output differs from the exact tandem queue schedule");
    bool caught = false;
    try { (void)ankurafathom::ir::load_file(invalid_branch_path); }
    catch (const ankurafathom::ir::Error& error) {
        caught = error.code == "IR_LINK" && error.pointer == "/links/2";
    }
    require(caught, "branching topology must be rejected with a link diagnostic");
}

void test_hybrid_completion(const std::string& path, const std::string& invalid_unit_path) {
    using namespace ankurafathom;
    const auto model = ir::load_file(path);
    require(model.kind == ir::Model::Kind::hybrid, "hybrid IR selected wrong runtime");
    const auto rows = ir::run(model);
    require(rows.size() == 10, "hybrid observation grid is wrong");

    // Independent direct composition of the typed DES and SD primitives.
    using Message = std::variant<des::Entity, double>;
    devs::Simulator<Message> simulator;
    const auto source = simulator.add(std::make_unique<des::ScheduledSource<Message>>(
        std::vector<des::Entity>{{1, 0, 1}, {2, 1, 1}, {3, 2.25, 0.25}}));
    const auto server = simulator.add(std::make_unique<des::MultiServer<Message>>(1));
    auto sink = std::make_unique<des::CompletionSink<Message>>();
    auto* sink_ptr = sink.get();
    const auto sink_id = simulator.add(std::move(sink));
    const auto bridge = simulator.add(std::make_unique<hybrid::EntityToPulse<Message>>(
        [](const des::Entity&) { return 1.0; }));
    sd::Model stocks;
    const auto stock = stocks.add_stock("completed_stock", 0);
    stocks.add_flow(stock, sd::Model::boundary,
        [stock](const auto& state, double) { return state[stock] * 0.5; });
    auto clocked = std::make_unique<hybrid::ClockedSD<Message>>(std::move(stocks), 1);
    auto* stock_ptr = clocked.get();
    const auto sd_id = simulator.add(std::move(clocked));
    simulator.connect(source, 0, server, 0);
    simulator.connect(server, 1, sink_id, 0);
    simulator.connect(server, 1, bridge, 0);
    simulator.connect(bridge, 1, sd_id, static_cast<std::uint32_t>(stock));

    const std::vector<double> exact_stock{0, 1, 1.5, 1.59375, 0.796875};
    const std::vector<double> exact_completed{0, 1, 2, 3, 3};
    std::vector<double> pulse_times;
    for (std::size_t step = 0; step < exact_stock.size(); ++step) {
        const auto trace = simulator.run_until(static_cast<double>(step));
        for (const auto& event : trace)
            for (const auto& emission : event.emissions)
                if (emission.source == bridge) pulse_times.push_back(event.time);
        const double direct_stock = stock_ptr->model().state()[stock];
        const double direct_completed = static_cast<double>(sink_ptr->completed_count());
        require(std::abs(direct_stock - exact_stock[step]) < 1e-12 &&
                direct_completed == exact_completed[step],
                "direct hybrid oracle differs from hand-derived event schedule");
        require(rows[2 * step].time == step && rows[2 * step].output_id == "stock_ts" &&
                std::abs(rows[2 * step].value - direct_stock) < 1e-12 &&
                rows[2 * step + 1].time == step &&
                rows[2 * step + 1].output_id == "finished" &&
                rows[2 * step + 1].value == direct_completed,
                "hybrid IR differs from direct typed composition");
    }
    require(pulse_times == std::vector<double>({1, 2, 2.5}),
            "hybrid bridge did not preserve on-grid and off-grid completion times");

    const auto no_decay = ir::run(model, {{"k", 0}});
    require(no_decay[6].value == 3 && no_decay[8].value == 3,
            "hybrid parameter override did not reach SD flow");
    bool caught = false;
    try { (void)ir::load_file(invalid_unit_path); }
    catch (const ir::Error& error) {
        caught = error.code == "IR_UNIT" && error.pointer == "/bridges/0/unit";
    }
    require(caught, "hybrid bridge amount unit must match target stock");
}

void test_stochastic_hybrid_conservation(const std::string& path) {
    const auto model = ankurafathom::ir::load_file(path);
    const auto first = ankurafathom::ir::run(model, {}, 987, 2, 0);
    const auto replay = ankurafathom::ir::run(model, {}, 987, 2, 0);
    const auto other = ankurafathom::ir::run(model, {}, 987, 2, 1);
    require(first.size() == 183 && replay.size() == first.size() &&
            other.size() == first.size(), "stochastic hybrid observation grid is wrong");
    bool differs = false;
    for (std::size_t i = 0; i < first.size(); ++i) {
        require(first[i].time == replay[i].time &&
                first[i].output_id == replay[i].output_id &&
                first[i].value == replay[i].value,
                "stochastic hybrid draw address did not replay exactly");
        differs |= first[i].value != other[i].value;
    }
    require(differs, "stochastic hybrid replication did not change its draw stream");
    for (const auto* rows : {&first, &other}) {
        for (std::size_t i = 0; i < rows->size(); i += 3) {
            require((*rows)[i].value == (*rows)[i + 1].value &&
                    (*rows)[i + 1].value <= (*rows)[i + 2].value,
                    "stochastic hybrid lost or duplicated a completion pulse");
        }
        require((*rows)[rows->size() - 3].value == 30 &&
                (*rows)[rows->size() - 2].value == 30,
                "stochastic hybrid did not complete all scheduled entities");
    }
}

void test_hybrid_feedback(const std::string& path, const std::string& overlap_path) {
    using namespace ankurafathom;
    const auto model = ir::load_file(path);
    require(model.feedback_bridge.has_value(), "feedback bridge missing from hybrid IR");
    const auto rows = ir::run(model);
    require(rows.size() == 15, "feedback observation grid is wrong");

    using Message = std::variant<des::Entity, double>;
    devs::Simulator<Message> simulator;
    const auto source = simulator.add(std::make_unique<des::ScheduledSource<Message>>(
        std::vector<des::Entity>{{1, 0, 1}}));
    auto station = std::make_unique<des::MultiServer<Message>>(1);
    auto* station_ptr = station.get();
    const auto station_id = simulator.add(std::move(station));
    auto sink = std::make_unique<des::CompletionSink<Message>>();
    auto* sink_ptr = sink.get();
    const auto sink_id = simulator.add(std::move(sink));
    const auto pulse_id = simulator.add(std::make_unique<hybrid::EntityToPulse<Message>>(
        [](const des::Entity&) { return 1.0; }));
    sd::Model stocks;
    const auto stock = stocks.add_stock("finished_stock", 0);
    auto clocked = std::make_unique<hybrid::ClockedSD<Message>>(std::move(stocks), 1);
    auto* stock_ptr = clocked.get();
    const auto sd_id = simulator.add(std::move(clocked));
    const auto feedback_id = simulator.add(std::make_unique<hybrid::StockToEntity<Message>>(
        1, 1, 100, 2));
    simulator.connect(source, 0, station_id, 0);
    simulator.connect(station_id, 1, sink_id, 0);
    simulator.connect(station_id, 1, pulse_id, 0);
    simulator.connect(pulse_id, 1, sd_id, static_cast<std::uint32_t>(stock));
    simulator.connect(feedback_id, 1, station_id, 0);

    const std::vector<double> expected_stock{0, 1, 2, 3, 3};
    const std::vector<double> expected_accepted{1, 2, 3, 3, 3};
    const std::vector<double> expected_completed{0, 1, 2, 3, 3};
    std::vector<std::pair<double, std::uint64_t>> feedback_entities;
    for (std::size_t step = 0; step < expected_stock.size(); ++step) {
        const double time = static_cast<double>(step);
        (void)simulator.run_until(time);
        simulator.inject(time, feedback_id, 0, Message{stock_ptr->model().state()[stock]});
        const auto trace = simulator.run_until(time);
        for (const auto& event : trace)
            for (const auto& emission : event.emissions)
                if (emission.source == feedback_id)
                    feedback_entities.emplace_back(event.time,
                        std::get<des::Entity>(emission.value).id);
        const double direct_stock = stock_ptr->model().state()[stock];
        const double direct_accepted = static_cast<double>(station_ptr->accepted_count());
        const double direct_completed = static_cast<double>(sink_ptr->completed_count());
        require(direct_stock == expected_stock[step] &&
                direct_accepted == expected_accepted[step] &&
                direct_completed == expected_completed[step],
                "direct feedback composition differs from hand-derived schedule");
        require(rows[3 * step].time == time && rows[3 * step].value == direct_stock &&
                rows[3 * step + 1].value == direct_accepted &&
                rows[3 * step + 2].value == direct_completed,
                "feedback IR differs from direct typed composition");
    }
    require(feedback_entities ==
            std::vector<std::pair<double, std::uint64_t>>({{1, 100}, {2, 101}}),
            "feedback entity times or IDs are wrong");
    bool caught = false;
    try { (void)ir::load_file(overlap_path); }
    catch (const ir::Error& error) {
        caught = error.code == "IR_HYBRID" && error.pointer == "/bridges/1/first_id";
    }
    require(caught, "feedback ID range must not overlap source IDs");
}

void test_hybrid_rate(const std::string& path, const std::string& invalid_unit_path,
                      const std::string& invalid_stream_path) {
    using namespace ankurafathom;
    const auto model = ir::load_file(path);
    require(model.rate_bridge.has_value(), "rate bridge missing from hybrid IR");
    bool distinct = false;
    std::vector<double> prior_counts;
    for (const std::uint64_t seed : {std::uint64_t{0}, std::uint64_t{123}}) {
        const auto rows = ir::run(model, {}, seed);
        const auto replay = ir::run(model, {}, seed);
        require(rows.size() == 15 && rows.size() == replay.size(),
                "rate-driven hybrid observation grid is wrong");
        double cumulative_hazard = 0;
        std::vector<double> event_thresholds;
        for (std::uint64_t id = 100; id < 105; ++id) {
            const auto arrival_word = rng::draw(seed,
                rng::DrawAddress{0, 0, id, 0, 20, 0})[0];
            cumulative_hazard += -std::log(rng::uniform_open(arrival_word));
            event_thresholds.push_back(cumulative_hazard);
            const auto service_word = rng::draw(seed,
                rng::DrawAddress{0, 0, id, 0, 21, 0})[0];
            require(rng::exponential(1e-9, service_word) > 4,
                    "chosen rate oracle seed would complete generated work inside horizon");
        }
        for (std::size_t step = 0; step <= 4; ++step) {
            const double integrated_rate = step == 0 ? 0 :
                step == 1 ? 1 : 1 + 2 * static_cast<double>(step - 1);
            const double expected_accepted = 1 + static_cast<double>(std::count_if(
                event_thresholds.begin(), event_thresholds.end(),
                [integrated_rate](double threshold) { return threshold <= integrated_rate; }));
            require(rows[3 * step].time == step &&
                    rows[3 * step].value == (step == 0 ? 1 : 2) &&
                    rows[3 * step + 1].value == expected_accepted &&
                    rows[3 * step + 2].value == (step == 0 ? 0 : 1),
                    "rate-driven IR differs from cumulative-hazard oracle");
            require(rows[3 * step].value == replay[3 * step].value &&
                    rows[3 * step + 1].value == replay[3 * step + 1].value &&
                    rows[3 * step + 2].value == replay[3 * step + 2].value,
                    "rate-driven hybrid did not replay exactly");
            if (seed == 0) prior_counts.push_back(rows[3 * step + 1].value);
            else distinct |= prior_counts[step] != rows[3 * step + 1].value;
        }
    }
    require(distinct, "different rate-source seeds yielded identical count trajectories");
    bool caught = false;
    try { (void)ir::load_file(invalid_unit_path); }
    catch (const ir::Error& error) {
        caught = error.code == "IR_UNIT" && error.pointer == "/bridges/1/gain_unit";
    }
    require(caught, "rate gain unit must map stock quantity to reciprocal time");
    caught = false;
    try { (void)ir::load_file(invalid_stream_path); }
    catch (const ir::Error& error) {
        caught = error.code == "IR_HYBRID" && error.pointer == "/bridges/1/stream";
    }
    require(caught, "rate-source Philox streams must not overlap source streams");
}

void test_abm_sd_adoption(const std::string& path, const std::string& invalid_unit_path) {
    using namespace ankurafathom;
    const auto model = ir::load_file(path);
    require(model.kind == ir::Model::Kind::abm_sd && model.abm_population.has_value(),
            "ABM-SD IR selected wrong runtime");
    constexpr std::uint64_t seed = 123;
    constexpr std::uint32_t scenario = 2;
    constexpr std::uint32_t replication = 1;
    const auto rows = ir::run(model, {}, seed, scenario, replication);
    const auto replay = ir::run(model, {}, seed, scenario, replication);
    const auto other = ir::run(model, {}, seed, scenario, replication + 1);
    require(rows.size() == 84 && replay.size() == rows.size() &&
            other.size() == rows.size(), "ABM-SD observation grid is wrong");

    abm::SyncPopulation<bool> population;
    for (std::size_t i = 0; i < 128; ++i) (void)population.spawn(i < 3);
    double probability = 0;
    std::uint32_t step_index = 0;
    population.add_phase([&](std::size_t index, const auto& snapshot) {
        if (snapshot[index].value) return true;
        const rng::DrawAddress address{scenario, replication, snapshot[index].id,
            step_index, 30, 0};
        return rng::bernoulli(probability, rng::draw(seed, address)[0]);
    });
    sd::Model stocks;
    const auto adopted = stocks.add_stock("adopted_stock", 0);
    const auto impact = stocks.add_stock("impact", 0);
    stocks.add_flow(sd::Model::boundary, impact,
        [adopted](const auto& state, double) { return 2 * state[adopted]; });
    auto clocked = std::make_unique<hybrid::ClockedSD<double>>(std::move(stocks), 0.1);
    auto* clocked_ptr = clocked.get();
    devs::Simulator<double> simulator;
    const auto sd_id = simulator.add(std::move(clocked));
    const auto bridge_id = simulator.add(
        std::make_unique<hybrid::PopulationToStock<bool>>(population,
            [](bool value) { return value ? 1.0 : 0.0; }));
    simulator.connect(bridge_id, 1, sd_id, static_cast<std::uint32_t>(adopted));

    bool differs = false;
    for (std::size_t step = 0; step <= 20; ++step) {
        const double time = static_cast<double>(step) * 0.1;
        (void)simulator.run_until(time);
        if (step > 0) {
            const auto prior = std::count_if(population.records().begin(), population.records().end(),
                [](const auto& agent) { return agent.value; });
            probability = (0.03 + 0.7 * static_cast<double>(prior) / 128) * 0.1;
            step_index = static_cast<std::uint32_t>(step);
            population.step();
        }
        simulator.inject(time, bridge_id, 0, 0.0);
        (void)simulator.run_until(time);
        const auto count = std::count_if(population.records().begin(), population.records().end(),
            [](const auto& agent) { return agent.value; });
        require(rows[4 * step].time == time &&
                rows[4 * step].value == static_cast<double>(count) &&
                rows[4 * step].value == clocked_ptr->model().state()[adopted] &&
                std::abs(rows[4 * step + 1].value - clocked_ptr->model().state()[impact]) < 1e-10 &&
                rows[4 * step + 2].value == 128 &&
                rows[4 * step + 3].value == static_cast<double>(count),
                "ABM-SD IR differs from direct typed composition");
        for (std::size_t column = 0; column < 4; ++column)
            require(rows[4 * step + column].value == replay[4 * step + column].value,
                    "ABM-SD run did not replay exactly");
        differs |= rows[4 * step].value != other[4 * step].value;
        if (step > 0)
            require(std::abs(rows[4 * step + 1].value -
                    (rows[4 * (step - 1) + 1].value +
                     0.2 * rows[4 * (step - 1)].value)) < 1e-10,
                    "SD impact flow did not read the prior committed adoption stock");
    }
    require(differs, "different ABM replication addresses yielded identical trajectories");
    const auto no_adoption = ir::run(model, {{"innovation", 0}, {"imitation", 0}});
    require(no_adoption.back().value == 3 &&
            std::abs(no_adoption[no_adoption.size() - 3].value - 12) < 1e-10,
            "scenario override did not reach ABM adoption rates");
    bool caught = false;
    try { (void)ir::run(model, {{"innovation", 10}}); }
    catch (const ir::Error& error) { caught = error.code == "IR_ABM_RUNTIME"; }
    require(caught, "out-of-range scenario adoption probability must fail");
    caught = false;
    try { (void)ir::load_file(invalid_unit_path); }
    catch (const ir::Error& error) {
        caught = error.code == "IR_UNIT" &&
            error.pointer == "/abm/population/innovation_param";
    }
    require(caught, "adoption rate parameter must have reciprocal-time units");
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 25) throw std::invalid_argument("expected twenty-four IR fixture paths");
        test_expression();
        test_loader_run_and_diagnostic(argv[1], argv[2], argv[3], argv[4]);
        test_lookup_model(argv[5], argv[6]);
        test_delay_models(argv[7], argv[8], argv[9]);
        test_des_process(argv[10], argv[11]);
        test_stochastic_des_process(argv[12]);
        test_tandem_des_process(argv[13], argv[14]);
        test_hybrid_completion(argv[15], argv[16]);
        test_stochastic_hybrid_conservation(argv[17]);
        test_hybrid_feedback(argv[18], argv[19]);
        test_hybrid_rate(argv[20], argv[21], argv[22]);
        test_abm_sd_adoption(argv[23], argv[24]);
        std::cout << "IR model tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
