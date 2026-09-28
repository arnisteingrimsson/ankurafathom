#include "harness.hpp"

#include <iostream>

using namespace conformance;
namespace {
Trace check(const Fixture& fixture) {
    auto result = run_native(fixture, true);
    equal(result, run_native(fixture, false), fixture.name + " nested/flat");
    equal(result, run_reference(fixture), fixture.name + " native/adevs");
    return result;
}

Component wrapped(Fixture& fixture, Definition definition, std::size_t depth) {
    const auto outer = fixture.group(root);
    auto inner = outer;
    for (std::size_t i = 1; i < depth; ++i) {
        const auto child = fixture.group(inner);
        fixture.connect(inner, boundary, 0, child, 0);
        fixture.connect(inner, child, 0, boundary, 0);
        inner = child;
    }
    const auto model = fixture.atomic(inner, definition);
    fixture.connect(inner, boundary, 0, model, 0);
    fixture.connect(inner, model, 0, boundary, 0);
    return outer;
}

void test_gpt() {
    Fixture fixture;
    fixture.name = "generator_processor_transducer";
    const auto generator = fixture.atomic(root, {Behavior::generator, 1, 4});
    const auto processor = wrapped(fixture, {Behavior::processor, 1.5, 1}, 4);
    const auto transducer = fixture.atomic(root, {Behavior::sink, 1, 1});
    fixture.connect(root, generator, 0, processor, 0);
    fixture.connect(root, generator, 0, transducer, 0);
    fixture.connect(root, processor, 0, transducer, 1);
    fixture.connect(root, processor, 0, boundary, 0);
    const auto trace = check(fixture);
    std::vector<std::pair<double, int>> completed;
    std::size_t seen = 0, confluent = 0;
    for (const auto& frame : trace) {
        for (const auto& [port, value] : frame.boundary) { (void)port; completed.emplace_back(frame.time, value); }
        for (const auto& transition : frame.transitions) {
            if (transition.component == transducer.id) seen += transition.input.size();
            if (transition.kind == 'C') ++confluent;
        }
    }
    require(completed == std::vector<std::pair<double, int>>{{2.5, 1}, {4, 2}, {5.5, 3}, {7, 4}} &&
            seen == 8 && confluent > 0, "GPT hand schedule or transducer conservation failed");
}

void test_confluent_policies() {
    for (const auto behavior : {Behavior::internal_first, Behavior::external_first}) {
        Fixture fixture;
        fixture.name = behavior == Behavior::internal_first ? "internal_then_external" : "external_then_internal";
        const auto model = wrapped(fixture, {behavior, 1, 1}, 3);
        fixture.connect(root, boundary, 0, model, 0);
        fixture.connect(root, model, 0, boundary, 0);
        fixture.inputs = {{1, 0, 3}};
        const auto trace = check(fixture);
        require(trace.size() == 2 && trace[0].time == 1 && trace[1].time == 1 &&
                trace[0].boundary == Bag{{0, 10}} && trace[0].transitions[0].kind == 'C' &&
                trace[1].boundary == Bag{{0, behavior == Behavior::internal_first ? 23 : 26}},
                "explicit confluent policy or pre-transition output changed");
    }
}

void test_zero_time_and_paths() {
    Fixture ping;
    ping.name = "ping_pong";
    const auto a = wrapped(ping, {Behavior::echo, 0, 1}, 2);
    const auto b = wrapped(ping, {Behavior::echo, 0, 1}, 3);
    ping.connect(root, boundary, 0, a, 0);
    ping.connect(root, a, 0, b, 0);
    ping.connect(root, b, 0, a, 0);
    ping.inputs = {{0, 0, 5}};
    const auto trace = check(ping);
    require(trace.size() == 6 && trace.back().time == 0 && trace.back().output[0].value == 0,
            "ping-pong zero-time microsteps were merged");

    Fixture chain;
    chain.name = "zero_time_chain_64";
    Component prior = boundary;
    for (std::size_t i = 0; i < 64; ++i) {
        const auto model = wrapped(chain, {Behavior::echo, 0, 1}, 2 + i % 4);
        chain.connect(root, prior, 0, model, 0);
        prior = model;
    }
    chain.connect(root, prior, 0, boundary, 0);
    chain.inputs = {{0.5, 0, 100}};
    const auto long_trace = check(chain);
    require(long_trace.size() == 65 && long_trace.back().boundary == Bag{{0, 36}},
            "long chain lost an event or advanced physical time");

    Fixture paths;
    paths.name = "distinct_boundary_paths";
    const auto left = paths.group(root), right = paths.group(root);
    paths.connect(left, boundary, 0, boundary, 0);
    paths.connect(right, boundary, 0, boundary, 0);
    const auto generator = paths.atomic(root, {Behavior::generator, 1, 1});
    const auto sink = paths.atomic(root, {Behavior::sink, 1, 1});
    paths.connect(root, generator, 0, left, 0);
    paths.connect(root, generator, 0, right, 0);
    paths.connect(root, left, 0, sink, 0);
    paths.connect(root, right, 0, sink, 0);
    const auto duplicated = check(paths);
    require(duplicated[0].transitions.back().input == Bag{{0, 1}, {0, 1}},
            "distinct paths must preserve duplicate payloads");
    auto corrupted = duplicated;
    corrupted[0].transitions.back().input.pop_back();
    require(corrupted != duplicated, "trace comparison lost bag multiplicity");
    corrupted = trace;
    corrupted.erase(corrupted.begin());
    require(corrupted != trace, "trace comparison lost microstep boundaries");
}

void test_larger_graphs() {
    for (std::size_t size : {8U, 32U, 128U})
        for (std::uint32_t seed = 0; seed < 8; ++seed) {
            Fixture fixture;
            fixture.name = "graph_n" + std::to_string(size) + "_seed" + std::to_string(seed);
            const auto first = fixture.atomic(root, {Behavior::generator, 0.5, 3});
            const auto second = fixture.atomic(root, {Behavior::generator, 1, 2});
            std::vector<Component> nodes;
            for (std::size_t i = 0; i < size; ++i)
                nodes.push_back(wrapped(fixture, {Behavior::echo, 0, 1}, 1 + i % 4));
            fixture.connect(root, first, 0, nodes[0], 0);
            fixture.connect(root, second, 0, nodes[1], 0);
            std::uint32_t random = seed;
            for (std::size_t i = 0; i < size; ++i) {
                // Activate every node, including those beyond the short-lived
                // generators' reach, and collide with the first source output.
                fixture.connect(root, boundary, 1, nodes[i], 0);
                fixture.connect(root, nodes[i], 0, boundary, 0);
                if (i + 1 < size) fixture.connect(root, nodes[i], 0, nodes[i + 1], 0);
                for (std::size_t j = i + 2; j < size; ++j) {
                    random = random * 1664525U + 1013904223U;
                    if (random >> 28 == 0) fixture.connect(root, nodes[i], 0, nodes[j], 0);
                }
            }
            fixture.connect(root, nodes.back(), 0, nodes[seed % 3], 0);
            fixture.inputs = {{0.5, 1, 3}};
            const auto trace = check(fixture);
            std::set<std::size_t> active;
            for (const auto& frame : trace)
                for (const auto& transition : frame.transitions) active.insert(transition.component);
            require(active.size() == fixture.atomics.size(), "larger graph left an atomic unexercised");
        }
}
} // namespace

int main() {
    try {
        test_gpt();
        test_confluent_policies();
        test_zero_time_and_paths();
        test_larger_graphs();
        std::cout << "30 conformance cases: GPT, confluent policies, ping-pong, zero-time chain, path multiplicity, 24 larger graphs\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
