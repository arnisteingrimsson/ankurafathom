// DEVStone topology/queue semantics adapted from SimulationEverywhere/devstone,
// commit 02fa99a8fe76bc61718a936089de3d63c1abbe71. See DEVSTONE_LICENSE.txt
// and README.md for provenance, workload policy, and trace comparison rules.
#include "harness.hpp"

#include <chrono>
#include <fstream>
#include <iostream>

#ifdef FATHOM_ALLOCATION_BENCH
#include "allocation_counter.hpp"
#endif

using namespace conformance;

namespace {
struct Measurement {
    Trace trace;
#ifdef FATHOM_ALLOCATION_BENCH
    fathom_bench::allocation::Counts allocations;
#endif
};
template<class Run> Measurement measure(Run run) {
#ifdef FATHOM_ALLOCATION_BENCH
    fathom_bench::allocation::Scope scope;
    auto trace = run();
    const auto counts = scope.counts();
    return {std::move(trace), counts};
#else
    return {run()};
#endif
}

void level(Fixture& fixture, Component parent, const std::string& family,
           std::size_t width, std::size_t depth, double period) {
    if (depth == 1) {
        const auto leaf = fixture.atomic(parent, {Behavior::devstone, period, 1});
        fixture.connect(parent, boundary, 0, leaf, 0);
        fixture.connect(parent, leaf, 0, boundary, 0);
        return;
    }
    const auto child = fixture.group(parent);
    level(fixture, child, family, width, depth - 1, period);
    fixture.connect(parent, boundary, 0, child, 0);
    fixture.connect(parent, child, 0, boundary, 0);
    if (family == "HO") fixture.connect(parent, boundary, 0, child, 1);
    if (family == "HOmod") {
        // Column c has c+2 atomics, directed from last to first. Both ends
        // receive input 1; each first atom triggers the inner level's input 1.
        for (std::size_t column = 0; column + 1 < width; ++column) {
            std::vector<Component> chain;
            for (std::size_t row = 0; row < column + 2; ++row)
                chain.push_back(fixture.atomic(parent, {Behavior::devstone, period, 1}));
            fixture.connect(parent, boundary, 1, chain.front(), 0);
            fixture.connect(parent, boundary, 1, chain.back(), 0);
            fixture.connect(parent, chain.front(), 0, child, 1);
            for (std::size_t row = 1; row < chain.size(); ++row)
                fixture.connect(parent, chain[row], 0, chain[row - 1], 0);
        }
    } else {
        std::vector<Component> siblings;
        for (std::size_t i = 1; i < width; ++i) {
            const auto atomic = fixture.atomic(parent, {Behavior::devstone, period, 1});
            fixture.connect(parent, boundary, family == "HO" ? 1 : 0, atomic, 0);
            if (family == "HO") fixture.connect(parent, atomic, 0, boundary, 1);
            if (family != "LI" && !siblings.empty())
                fixture.connect(parent, siblings.back(), 0, atomic, 0);
            siblings.push_back(atomic);
        }
    }
}

Fixture devstone(const std::string& family, std::size_t width, std::size_t depth, double period) {
    Fixture fixture;
    fixture.name = family + "_w" + std::to_string(width) + "_d" + std::to_string(depth) +
                   "_ta" + std::to_string(static_cast<int>(period));
    level(fixture, root, family, width, depth, period);
    for (double time : {0.0, 0.0, 1.0}) {
        fixture.inputs.push_back({time, 0, 1});
        if ((family == "HO" || family == "HOmod") && depth > 1 && width > 1)
            fixture.inputs.push_back({time, 1, 1});
    }
    return fixture;
}

std::size_t expected_outputs(const std::string& family, std::size_t width, std::size_t depth) {
    const std::size_t n = width - 1;
    if (family == "LI") return 3 * (1 + n * (depth - 1));
    if (family != "HOmod") return 3 * (1 + n * (n + 1) / 2 * (depth - 1));
    std::size_t total = 1, incoming = 1;
    for (std::size_t i = 1; i < depth; ++i) {
        total += incoming * n * (n + 5) / 2;
        incoming *= 2 * n;
    }
    return 3 * total;
}

void counts(const Fixture& fixture, const Trace& trace, const std::string& family,
            std::size_t width, std::size_t depth) {
    const auto n = width - 1;
    const auto per_level = family == "HOmod" ? n * (n + 3) / 2 : n;
    require(fixture.atomics.size() == 1 + per_level * (depth - 1), "DEVStone atomic-count oracle failed");
    std::size_t outputs = 0, internal = 0, input_messages = 0, boundary_primary = 0, boundary_secondary = 0;
    for (const auto& frame : trace) {
        outputs += frame.output.size();
        for (const auto& transition : frame.transitions) {
            if (transition.kind != 'E') ++internal;
            input_messages += transition.input.size();
        }
        for (const auto& [port, value] : frame.boundary) {
            require(value == 1, "DEVStone output value changed");
            (port == 0 ? boundary_primary : boundary_secondary)++;
        }
    }
    const auto expected = expected_outputs(family, width, depth);
    require(outputs == expected && internal == expected && input_messages == expected,
            fixture.name + ": analytic message/transition count failed");
    require(boundary_primary == 3 && boundary_secondary ==
            (family == "HO" && depth > 1 ? 3 * n * (n + 1) / 2 : 0),
            fixture.name + ": boundary-output count failed");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: devs_devstone_tests timing.csv");
        std::ofstream csv(argv[1]);
        require(static_cast<bool>(csv), "cannot write DEVStone timing report");
        csv << "case,atomics,steps,outputs,nested_us,flat_us,adevs_us";
#ifdef FATHOM_ALLOCATION_BENCH
        csv << ",nested_allocations,nested_requested_bytes,flat_allocations,flat_requested_bytes,adevs_allocations,adevs_requested_bytes";
#endif
        csv << ",trace_equal\n";
        std::size_t cases = 0;
        for (const std::string family : {"LI", "HI", "HO", "HOmod"})
            for (const auto [width, depth] : std::vector<std::pair<std::size_t, std::size_t>>{
                     {1, 1}, {2, 2}, {3, 3}, {5, 3}, {3, 6}, {8, 4}})
                for (double period : {0.0, 1.0}) {
                    const auto fixture = devstone(family, width, depth, period);
                    using Clock = std::chrono::steady_clock;
                    const auto start = Clock::now();
                    const auto nested_run = measure([&] { return run_native(fixture, true); });
                    const auto next = Clock::now();
                    const auto flat_run = measure([&] { return run_native(fixture, false); });
                    const auto third = Clock::now();
                    const auto reference_run = measure([&] { return run_reference(fixture); });
                    const auto end = Clock::now();
                    const auto& nested = nested_run.trace;
                    const auto& flat = flat_run.trace;
                    const auto& reference = reference_run.trace;
                    equal(nested, flat, fixture.name + " nested/flat");
                    equal(nested, reference, fixture.name + " native/adevs");
                    counts(fixture, nested, family, width, depth);
                    std::size_t events = 0;
                    for (const auto& step : nested) events += step.output.size();
                    auto us = [](auto a, auto b) {
                        return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
                    };
                    csv << fixture.name << ',' << fixture.atomics.size() << ',' << nested.size() << ',' << events
                        << ',' << us(start, next) << ',' << us(next, third) << ',' << us(third, end);
#ifdef FATHOM_ALLOCATION_BENCH
                    for (const auto totals : {nested_run.allocations, flat_run.allocations, reference_run.allocations}) {
                        require(!totals.overflow, "allocation counter overflow; report would be invalid");
                        csv << ',' << totals.allocations << ',' << totals.requested_bytes;
                    }
#endif
                    csv << ",true\n";
                    ++cases;
                }
        csv.close();
        require(static_cast<bool>(csv), "failed writing DEVStone report");
        std::cout << cases << " DEVStone cases: native nested = native flat = pinned adevs; analytic counts pass\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
