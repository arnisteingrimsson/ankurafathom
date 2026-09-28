#include "ankurafathom/economics.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool close(double a, double b, double tolerance = 1e-8) {
    return std::abs(a - b) <= tolerance * std::max({1.0, std::abs(a), std::abs(b)});
}

ankurafathom::Practice specimen() {
    ankurafathom::Practice p;
    p.name = "Test";
    p.fte = 10;
    p.paid_hours_per_fte_month = 160;
    p.delivery_share = 0.75;
    p.realized_rate = 200;
    p.monthly_pipeline_hours = 800;
    p.win_rate = 1;
    p.fixed_fee_share = 0;
    p.annual_pay_per_fte = 100000;
    p.variable_cost_per_hour = 100;
    p.ai_task_share = 1;
    return p;
}

void test_tm_without_slack() {
    const auto p = specimen();
    const auto base = ankurafathom::simulate(p, 0);
    const auto ai = ankurafathom::simulate(p, ankurafathom::copilot);
    require(ai.monthly[11].revenue < base.monthly[11].revenue,
            "T&M productivity must reduce billed revenue when demand is fixed and capacity is slack");
}

void test_tm_with_demand() {
    auto p = specimen();
    p.monthly_pipeline_hours = 1500;
    const auto base = ankurafathom::simulate(p, 0);
    const auto ai = ankurafathom::simulate(p, ankurafathom::copilot);
    require(close(base.monthly[11].revenue, ai.monthly[11].revenue),
            "T&M with deep backlog should refill billed capacity");
    require(ai.monthly[11].tm_delivered_hours > base.monthly[11].tm_delivered_hours,
            "AI should complete more engagements when work is available");
}

void test_fixed_fee() {
    auto p = specimen();
    p.fixed_fee_share = 1;
    const auto base = ankurafathom::simulate(p, 0);
    const auto ai = ankurafathom::simulate(p, ankurafathom::copilot);
    require(close(base.monthly[11].revenue, ai.monthly[11].revenue),
            "fixed-fee contract revenue must not change with labor productivity");
    require(ai.monthly[11].profit > base.monthly[11].profit,
            "fixed-fee variable delivery cost savings should exceed Copilot cost in specimen");
}

void test_accounting() {
    auto p = specimen();
    p.fixed_fee_share = 0.4;
    p.monthly_pipeline_hours = 1500;
    p.initial_tm_backlog_hours = 100;
    p.initial_fixed_backlog_hours = 50;
    const auto run = ankurafathom::simulate(p, ankurafathom::copilot | ankurafathom::automation);
    double tm_start = p.initial_tm_backlog_hours;
    double fixed_start = p.initial_fixed_backlog_hours;
    for (const auto& row : run.monthly) {
        require(close(row.tm_backlog_hours, tm_start + row.tm_won_hours - row.tm_delivered_hours),
                "T&M backlog does not conserve work");
        require(close(row.fixed_backlog_hours, fixed_start + row.fixed_won_hours - row.fixed_delivered_hours),
                "fixed-fee backlog does not conserve work");
        require(row.tm_backlog_hours >= 0 && row.fixed_backlog_hours >= 0, "negative backlog");
        require(row.actual_delivery_hours <= row.paid_hours * p.delivery_share + 1e-8,
                "delivery exceeds capacity");
        require(close(row.revenue - row.cost, row.profit), "profit identity fails");
        tm_start = row.tm_backlog_hours;
        fixed_start = row.fixed_backlog_hours;
    }
}

void test_no_demand_or_capacity() {
    auto p = specimen();
    p.monthly_pipeline_hours = 0;
    require(close(ankurafathom::simulate(p, 0).monthly[0].revenue, 0), "no demand generated revenue");
    p.monthly_pipeline_hours = 800;
    p.delivery_share = 0;
    require(close(ankurafathom::simulate(p, 0).monthly[0].revenue, 0), "no capacity generated revenue");
}

void test_acquisition_and_firm_rollup() {
    const auto p = specimen();
    const auto acquired = ankurafathom::simulate(p, ankurafathom::acquisition);
    require(close(acquired.monthly[11].fte, p.fte), "acquisition changed headcount before month 13");
    require(close(acquired.monthly[12].fte, p.fte * 1.15), "acquisition headcount did not arrive in month 13");
    const auto rows = ankurafathom::annualize({acquired, acquired}, "acquisition");
    require(rows.size() == 15, "annual practice and firm row count is wrong");
    require(close(rows[0].revenue + rows[1].revenue, rows[2].revenue),
            "firm revenue does not equal practice sum");
    require(close(rows[0].profit + rows[1].profit, rows[2].profit),
            "firm profit does not equal practice sum");
}

void test_synthetic_practice_profiles(const std::string& input_path) {
    const auto practices = ankurafathom::load_practices_csv(input_path);
    require(practices.size() == 2, "synthetic pilot must contain two practices");
    const auto disputes_base = ankurafathom::simulate(practices[0], 0);
    const auto disputes_ai = ankurafathom::simulate(practices[0], ankurafathom::copilot);
    const auto advisory_base = ankurafathom::simulate(practices[1], 0);
    const auto advisory_ai = ankurafathom::simulate(practices[1], ankurafathom::copilot);
    require(close(disputes_base.monthly[59].tm_backlog_hours, 0),
            "synthetic Disputes should have demand slack by the final year");
    require(advisory_base.monthly[59].fixed_backlog_hours > 0,
            "synthetic Advisory should have a fixed-fee backlog");
    require(disputes_ai.monthly[59].revenue < disputes_base.monthly[59].revenue,
            "Copilot should reduce Disputes billed revenue under demand slack");
    require(advisory_ai.monthly[11].revenue > advisory_base.monthly[11].revenue,
            "Copilot should deliver more work in Advisory with backlog");
}

} // namespace

int main(int argc, char** argv) {
    try {
        test_tm_without_slack();
        test_tm_with_demand();
        test_fixed_fee();
        test_accounting();
        test_no_demand_or_capacity();
        test_acquisition_and_firm_rollup();
        if (argc == 2) test_synthetic_practice_profiles(argv[1]);
        std::cout << "economics tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
