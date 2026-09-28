#include "ankurafathom/economics.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <numeric>
#include <sstream>
#include <stdexcept>

namespace ankurafathom {
namespace {

constexpr double copilot_effect = 0.08;
constexpr double tool_effect = 0.15;
constexpr double automation_effect = 0.30 * 0.85; // task effect × straight-through rate
constexpr double acquisition_fte_share = 0.15;
constexpr double acquisition_pipeline_share = 0.20;

std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> cells;
    std::stringstream input(line);
    std::string cell;
    while (std::getline(input, cell, ',')) cells.push_back(cell);
    if (!line.empty() && line.back() == ',') cells.emplace_back();
    return cells;
}

double parse_number(const std::string& cell, std::size_t line_number) {
    try {
        std::size_t used = 0;
        const double value = std::stod(cell, &used);
        if (used != cell.size() || !std::isfinite(value)) throw std::invalid_argument("invalid number");
        return value;
    } catch (const std::exception&) {
        throw std::runtime_error("invalid numeric value on CSV line " + std::to_string(line_number));
    }
}

double ramp(int month, int first_month, int duration) {
    if (month < first_month) return 0;
    return std::min(1.0, static_cast<double>(month - first_month + 1) / duration);
}

double labor_factor(const Practice& p, std::uint8_t flags, int month) {
    double factor = 1;
    if (flags & copilot) factor *= 1 - copilot_effect * p.ai_task_share * ramp(month, 2, 6);
    if (flags & tool_build) factor *= 1 - tool_effect * p.tool_task_share * ramp(month, 10, 6);
    if (flags & automation) factor *= 1 - automation_effect * p.automation_task_share * ramp(month, 7, 6);
    return std::max(0.5, factor);
}

double intervention_cost(const Practice& p, std::uint8_t flags, int month, double fte) {
    double cost = 0;
    if (flags & copilot) {
        cost += 40 * fte;
        if (month == 1) cost += 300 * p.fte;
    }
    if (flags & tool_build) {
        if (month == 1) cost += 750000;
        if (month >= 10) cost += 5000;
    }
    if (flags & automation) {
        if (month == 1) cost += 250000;
        if (month >= 7) cost += 3000;
    }
    if (flags & acquisition) {
        if (month == 13) cost += 2000000;
        if (month >= 13 && month < 31) cost += 20000;
    }
    return cost;
}

AnnualResult annual_for(const PracticeRun& run, const std::string& scenario, int year) {
    AnnualResult result;
    result.scenario = scenario;
    result.practice = run.name;
    result.year = year;
    double delivered = 0;
    double paid = 0;
    for (int month = (year - 1) * 12; month < year * 12; ++month) {
        const auto& row = run.monthly[static_cast<std::size_t>(month)];
        result.revenue += row.revenue;
        result.cost += row.cost;
        result.profit += row.profit;
        delivered += row.actual_delivery_hours;
        paid += row.paid_hours;
    }
    const auto& ending = run.monthly[static_cast<std::size_t>(year * 12 - 1)];
    result.headcount = ending.fte;
    result.tm_backlog_hours = ending.tm_backlog_hours;
    result.fixed_backlog_hours = ending.fixed_backlog_hours;
    result.utilization = paid > 0 ? delivered / paid : 0;
    result.margin = result.revenue > 0 ? result.profit / result.revenue : 0;
    return result;
}

} // namespace

void validate(const Practice& p) {
    if (p.name.empty()) throw std::invalid_argument("practice name is empty");
    const double nonnegative[] = {p.fte, p.paid_hours_per_fte_month, p.realized_rate,
                                  p.monthly_pipeline_hours, p.annual_pay_per_fte,
                                  p.variable_cost_per_hour, p.initial_tm_backlog_hours,
                                  p.initial_fixed_backlog_hours};
    for (double value : nonnegative) {
        if (!std::isfinite(value) || value < 0) throw std::invalid_argument("negative or non-finite practice input: " + p.name);
    }
    const double fractions[] = {p.delivery_share, p.win_rate, p.fixed_fee_share,
                                p.ai_task_share, p.tool_task_share, p.automation_task_share};
    for (double value : fractions) {
        if (!std::isfinite(value) || value < 0 || value > 1) throw std::invalid_argument("fraction outside [0,1]: " + p.name);
    }
    if (!std::isfinite(p.annual_demand_growth) || p.annual_demand_growth <= -1)
        throw std::invalid_argument("annual demand growth must be greater than -1: " + p.name);
}

std::vector<Practice> load_practices_csv(const std::string& path) {
    std::ifstream file(path);
    if (!file) throw std::runtime_error("cannot open input: " + path);
    std::string line;
    if (!std::getline(file, line)) throw std::runtime_error("empty input: " + path);
    const std::string expected = "name,fte,paid_hours_per_fte_month,delivery_share,realized_rate,monthly_pipeline_hours,win_rate,fixed_fee_share,annual_pay_per_fte,variable_cost_per_hour,annual_demand_growth,ai_task_share,tool_task_share,automation_task_share,initial_tm_backlog_hours,initial_fixed_backlog_hours";
    if (line != expected) throw std::runtime_error("unexpected CSV header in " + path);
    std::vector<Practice> practices;
    std::size_t line_number = 1;
    while (std::getline(file, line)) {
        ++line_number;
        if (line.empty()) continue;
        const auto cells = split_csv_line(line);
        if (cells.size() != 16) throw std::runtime_error("expected 16 columns on CSV line " + std::to_string(line_number));
        Practice p;
        p.name = cells[0];
        double* fields[] = {&p.fte, &p.paid_hours_per_fte_month, &p.delivery_share, &p.realized_rate,
                            &p.monthly_pipeline_hours, &p.win_rate, &p.fixed_fee_share,
                            &p.annual_pay_per_fte, &p.variable_cost_per_hour, &p.annual_demand_growth,
                            &p.ai_task_share, &p.tool_task_share, &p.automation_task_share,
                            &p.initial_tm_backlog_hours, &p.initial_fixed_backlog_hours};
        for (std::size_t i = 0; i < 15; ++i) *fields[i] = parse_number(cells[i + 1], line_number);
        validate(p);
        for (const auto& prior : practices) {
            if (prior.name == p.name) throw std::runtime_error("duplicate practice name: " + p.name);
        }
        practices.push_back(std::move(p));
    }
    if (practices.empty()) throw std::runtime_error("input has no practices: " + path);
    return practices;
}

std::string scenario_name(std::uint8_t flags) {
    if (flags == 0) return "baseline";
    std::string name;
    const auto append = [&name](const char* part) {
        if (!name.empty()) name += '+';
        name += part;
    };
    if (flags & copilot) append("copilot");
    if (flags & tool_build) append("tool");
    if (flags & automation) append("automation");
    if (flags & acquisition) append("acquisition");
    return name;
}

PracticeRun simulate(const Practice& p, std::uint8_t flags) {
    validate(p);
    if (flags > 15) throw std::invalid_argument("unknown intervention bit");
    PracticeRun run;
    run.name = p.name;
    double tm_backlog = p.initial_tm_backlog_hours;
    double fixed_backlog = p.initial_fixed_backlog_hours;
    for (int month = 1; month <= months; ++month) {
        auto& row = run.monthly[static_cast<std::size_t>(month - 1)];
        const bool acquired = (flags & acquisition) && month >= 13;
        row.fte = p.fte * (acquired ? 1 + acquisition_fte_share : 1);
        row.paid_hours = row.fte * p.paid_hours_per_fte_month;

        const double demand_factor = std::pow(1 + p.annual_demand_growth, (month - 1) / 12.0);
        const double pipeline = p.monthly_pipeline_hours * demand_factor *
                                (acquired ? 1 + acquisition_pipeline_share : 1);
        const double won = pipeline * p.win_rate;
        row.fixed_won_hours = won * p.fixed_fee_share;
        row.tm_won_hours = won - row.fixed_won_hours;
        tm_backlog += row.tm_won_hours;
        fixed_backlog += row.fixed_won_hours;

        const double factor = labor_factor(p, flags, month);
        const double actual_hours_needed = (tm_backlog + fixed_backlog) * factor;
        const double capacity = row.paid_hours * p.delivery_share;
        const double share_delivered = actual_hours_needed > 0 ? std::min(1.0, capacity / actual_hours_needed) : 0;
        row.tm_delivered_hours = tm_backlog * share_delivered;
        row.fixed_delivered_hours = fixed_backlog * share_delivered;
        tm_backlog = std::max(0.0, tm_backlog - row.tm_delivered_hours);
        fixed_backlog = std::max(0.0, fixed_backlog - row.fixed_delivered_hours);
        row.tm_backlog_hours = tm_backlog;
        row.fixed_backlog_hours = fixed_backlog;
        row.actual_delivery_hours = (row.tm_delivered_hours + row.fixed_delivered_hours) * factor;
        row.revenue = (row.tm_delivered_hours * factor + row.fixed_delivered_hours) * p.realized_rate;
        row.cost = row.fte * p.annual_pay_per_fte / 12 +
                   row.actual_delivery_hours * p.variable_cost_per_hour +
                   intervention_cost(p, flags, month, row.fte);
        row.profit = row.revenue - row.cost;
    }
    return run;
}

std::vector<AnnualResult> annualize(const std::vector<PracticeRun>& runs, const std::string& scenario) {
    if (runs.empty()) throw std::invalid_argument("no practice runs");
    std::vector<AnnualResult> results;
    for (int year = 1; year <= years; ++year) {
        AnnualResult firm;
        firm.scenario = scenario;
        firm.practice = "FIRM";
        firm.year = year;
        double delivered = 0;
        double paid = 0;
        for (const auto& run : runs) {
            auto row = annual_for(run, scenario, year);
            results.push_back(row);
            firm.revenue += row.revenue;
            firm.cost += row.cost;
            firm.profit += row.profit;
            firm.headcount += row.headcount;
            firm.tm_backlog_hours += row.tm_backlog_hours;
            firm.fixed_backlog_hours += row.fixed_backlog_hours;
            for (int month = (year - 1) * 12; month < year * 12; ++month) {
                delivered += run.monthly[static_cast<std::size_t>(month)].actual_delivery_hours;
                paid += run.monthly[static_cast<std::size_t>(month)].paid_hours;
            }
        }
        firm.utilization = paid > 0 ? delivered / paid : 0;
        firm.margin = firm.revenue > 0 ? firm.profit / firm.revenue : 0;
        results.push_back(firm);
    }
    return results;
}

ScenarioSummary summarize(const std::vector<PracticeRun>& baseline,
                          const std::vector<PracticeRun>& scenario,
                          const std::string& name) {
    if (baseline.size() != scenario.size()) throw std::invalid_argument("practice count mismatch");
    ScenarioSummary result;
    result.scenario = name;
    double cumulative = 0;
    bool had_deficit = false;
    std::array<double, months> cumulative_by_month{};
    std::array<bool, months> deficit_before_or_at{};
    for (int month = 0; month < months; ++month) {
        double delta = 0;
        for (std::size_t i = 0; i < baseline.size(); ++i) {
            if (baseline[i].name != scenario[i].name) throw std::invalid_argument("practice order mismatch");
            delta += scenario[i].monthly[static_cast<std::size_t>(month)].profit -
                     baseline[i].monthly[static_cast<std::size_t>(month)].profit;
        }
        result.incremental_npv += delta / std::pow(1.10, (month + 1) / 12.0);
        cumulative += delta;
        if (cumulative < 0) had_deficit = true;
        cumulative_by_month[static_cast<std::size_t>(month)] = cumulative;
        deficit_before_or_at[static_cast<std::size_t>(month)] = had_deficit;
    }
    for (int month = 0; month < months; ++month) {
        if (!deficit_before_or_at[static_cast<std::size_t>(month)] ||
            cumulative_by_month[static_cast<std::size_t>(month)] < 0) continue;
        bool remains_paid_back = true;
        for (int later = month + 1; later < months; ++later) {
            if (cumulative_by_month[static_cast<std::size_t>(later)] < 0) {
                remains_paid_back = false;
                break;
            }
        }
        if (remains_paid_back) {
            result.payback_month = month + 1;
            break;
        }
    }
    return result;
}

} // namespace ankurafathom
