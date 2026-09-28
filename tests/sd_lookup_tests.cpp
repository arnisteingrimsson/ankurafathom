#include "ankurafathom/sd/lookup_table.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_interpolation_and_boundaries() {
    using ankurafathom::sd::Extrapolation;
    using ankurafathom::sd::LookupTable;
    const LookupTable clamped({0, 1, 3}, {0, 10, 20}, Extrapolation::clamp);
    require(clamped.evaluate(0) == 0 && clamped.evaluate(1) == 10 &&
            clamped.evaluate(3) == 20, "lookup knots must be exact");
    require(clamped.evaluate(0.5) == 5 && clamped.evaluate(2) == 15,
            "piecewise-linear interpolation is wrong");
    require(clamped.evaluate(-1) == 0 && clamped.evaluate(4) == 20,
            "clamped extrapolation is wrong");
    const LookupTable linear({0, 1, 3}, {0, 10, 20}, Extrapolation::linear);
    require(linear.evaluate(-1) == -10 && linear.evaluate(4) == 25,
            "linear extrapolation is wrong");
}

void test_invalid_table_and_query() {
    using ankurafathom::sd::Extrapolation;
    using ankurafathom::sd::LookupTable;
    bool caught = false;
    try { (void)LookupTable({0, 0}, {1, 2}, Extrapolation::clamp); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught, "duplicate x knots must fail");
    caught = false;
    try { (void)LookupTable({0}, {1}, Extrapolation::clamp); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught, "single-knot table must fail");
    const LookupTable valid({0, 1}, {0, 1}, Extrapolation::clamp);
    caught = false;
    try { (void)valid.evaluate(std::numeric_limits<double>::quiet_NaN()); }
    catch (const std::invalid_argument&) { caught = true; }
    require(caught, "nonfinite lookup query must fail");
}

} // namespace

int main() {
    try {
        test_interpolation_and_boundaries();
        test_invalid_table_and_query();
        std::cout << "SD lookup tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
