#include "ankurafathom/sd/model.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

double decay(ankurafathom::sd::Integrator method, double dt) {
    ankurafathom::sd::Model model;
    const auto stock = model.add_stock("material", 100);
    model.add_flow(stock, ankurafathom::sd::Model::boundary,
                   [stock](const auto& state, double) { return 0.2 * state[stock]; });
    const int steps = static_cast<int>(1 / dt);
    for (int i = 0; i < steps; ++i) model.step(i * dt, dt, method);
    return model.state()[stock];
}

void test_integrators() {
    using ankurafathom::sd::Integrator;
    const double euler = decay(Integrator::euler, 0.1);
    require(std::abs(euler - 100 * std::pow(0.98, 10)) < 1e-10,
            "Euler must match its exact discrete recurrence");
    const double exact = 100 * std::exp(-0.2);
    const double midpoint_coarse = std::abs(decay(Integrator::midpoint, 0.1) - exact);
    const double midpoint_fine = std::abs(decay(Integrator::midpoint, 0.05) - exact);
    require(midpoint_fine < midpoint_coarse / 3.5, "midpoint should converge at second order");
    const double rk4_coarse = std::abs(decay(Integrator::rk4, 0.1) - exact);
    const double rk4_fine = std::abs(decay(Integrator::rk4, 0.05) - exact);
    require(rk4_fine < rk4_coarse / 14, "RK4 should converge at fourth order");
}

void test_conservation_and_simultaneous_commit() {
    ankurafathom::sd::Model model;
    const auto first = model.add_stock("first", 80);
    const auto second = model.add_stock("second", 20);
    model.add_flow(first, second, [first](const auto& state, double) { return state[first] * 0.1; });
    model.add_flow(second, first, [second](const auto& state, double) { return state[second] * 0.05; });
    const auto derivative = model.derivative(model.state(), 0);
    require(std::abs(derivative[first] + derivative[second]) < 1e-12,
            "closed stock system must have zero net derivative");
    for (int i = 0; i < 100; ++i) model.step(i * 0.1, 0.1);
    require(std::abs(model.state()[first] + model.state()[second] - 100) < 1e-9,
            "closed stock system must conserve material");
}

void test_error_paths() {
    ankurafathom::sd::Model model;
    const auto stock = model.add_stock("stock", 1);
    model.add_flow(stock, ankurafathom::sd::Model::boundary,
                   [](const auto&, double) { return 2.0; });
    bool caught = false;
    try { model.step(0, 1); } catch (const std::domain_error&) { caught = true; }
    require(caught && model.state()[stock] == 1, "negative stock must reject without committing");
    caught = false;
    try { model.step(0, 0); } catch (const std::invalid_argument&) { caught = true; }
    require(caught, "zero-length step must be rejected");
}

void test_signed_flows() {
    using namespace ankurafathom::sd;
    for (auto method : {Integrator::euler, Integrator::midpoint, Integrator::rk4}) {
        Model model;
        auto a = model.add_stock("a", 10);
        auto b = model.add_stock("b", 10);
        model.add_flow(a, b, [](const auto&, double t) { return t-1; }, false);
        model.step(0, 1, method);
        const double transfer = method == Integrator::euler ? -1 : -0.5;
        require(model.state()[a] == 10-transfer && model.state()[b] == 10+transfer,
                "signed transfer must conserve total and reverse direction");
        model.step(1, 1, method);
        require(model.state()[a]+model.state()[b] == 20, "signed conservation");
    }
    for (bool signed_flow : {false, true}) {
        Model model;
        auto stock = model.add_stock("nonnegative", 1);
        model.add_flow(Model::boundary, stock, [](const auto&, double) { return -2.; }, !signed_flow);
        bool caught = false;
        try { model.step(0, 1); } catch (const std::domain_error&) { caught = true; }
        require(caught && model.state()[stock] == 1,
                "default flow sign and stock bounds must each reject without committing");
    }
    Model signed_stock;
    auto stock = signed_stock.add_stock("signed", 1, false);
    signed_stock.add_flow(Model::boundary, stock, [](const auto&, double) { return -2.; }, false);
    signed_stock.step(0, 1);
    require(signed_stock.state()[stock] == -1, "signed stock and signed flow are independent options");
    Model invalid;
    auto id = invalid.add_stock("finite", 0, false);
    invalid.add_flow(Model::boundary, id, [](const auto&, double t) {
        return t == 0 ? 1. : std::numeric_limits<double>::infinity();
    }, false);
    bool caught = false;
    try { invalid.step(0, 1, Integrator::rk4); } catch (const std::domain_error&) { caught = true; }
    require(caught && invalid.state()[id] == 0, "nonfinite signed RK stage must roll back");
}

} // namespace

int main() {
    try {
        test_integrators();
        test_conservation_and_simultaneous_commit();
        test_error_paths();
        test_signed_flows();
        std::cout << "SD model tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
