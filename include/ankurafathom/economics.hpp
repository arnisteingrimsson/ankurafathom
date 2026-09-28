#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ankurafathom {

constexpr int months = 60;
constexpr int years = months / 12;

enum Intervention : std::uint8_t {
    copilot = 1,
    tool_build = 2,
    automation = 4,
    acquisition = 8,
};

struct Practice {
    std::string name;
    double fte = 0;                          // people
    double paid_hours_per_fte_month = 0;     // hours / person / month
    double delivery_share = 0;               // fraction of paid time available for delivery
    double realized_rate = 0;                // USD / billed or baseline hour
    double monthly_pipeline_hours = 0;       // prospective baseline hours / month
    double win_rate = 0;                     // fraction
    double fixed_fee_share = 0;              // fraction of won baseline hours
    double annual_pay_per_fte = 0;           // USD / person / year
    double variable_cost_per_hour = 0;       // USD / actual delivery hour
    double annual_demand_growth = 0;         // fraction / year
    double ai_task_share = 0;                // fraction of work exposed to Copilot
    double tool_task_share = 0;              // fraction of work exposed to internal tool
    double automation_task_share = 0;        // fraction of work exposed to automation
    double initial_tm_backlog_hours = 0;     // baseline hours
    double initial_fixed_backlog_hours = 0;  // baseline hours
};

struct MonthResult {
    double revenue = 0;
    double cost = 0;
    double profit = 0;
    double actual_delivery_hours = 0;
    double paid_hours = 0;
    double fte = 0;
    double tm_backlog_hours = 0;
    double fixed_backlog_hours = 0;
    double tm_won_hours = 0;
    double fixed_won_hours = 0;
    double tm_delivered_hours = 0;           // baseline hours
    double fixed_delivered_hours = 0;        // baseline hours
};

struct PracticeRun {
    std::string name;
    std::array<MonthResult, months> monthly{};
};

struct AnnualResult {
    std::string scenario;
    std::string practice;
    int year = 0;
    double revenue = 0;
    double cost = 0;
    double profit = 0;
    double margin = 0;
    double utilization = 0;
    double headcount = 0;
    double tm_backlog_hours = 0;
    double fixed_backlog_hours = 0;
};

struct ScenarioSummary {
    std::string scenario;
    double incremental_npv = 0;
    int payback_month = 0; // 0 means no payback within the horizon or no initial deficit
};

std::vector<Practice> load_practices_csv(const std::string& path);
void validate(const Practice& practice);
std::string scenario_name(std::uint8_t interventions);
PracticeRun simulate(const Practice& practice, std::uint8_t interventions);
std::vector<AnnualResult> annualize(const std::vector<PracticeRun>& runs, const std::string& scenario);
ScenarioSummary summarize(const std::vector<PracticeRun>& baseline,
                          const std::vector<PracticeRun>& scenario,
                          const std::string& scenario_name);

} // namespace ankurafathom
