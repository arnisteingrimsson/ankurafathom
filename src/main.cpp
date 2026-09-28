#include "ankurafathom/economics.hpp"
#include "ankurafathom/ir/model.hpp"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void write_yearly(std::ostream& out, const std::vector<ankurafathom::AnnualResult>& rows) {
    out << "scenario,practice,year,revenue_usd,cost_usd,operating_profit_usd,margin,utilization,headcount_fte,tm_backlog_baseline_hours,fixed_backlog_baseline_hours\n";
    out << std::fixed << std::setprecision(6);
    for (const auto& r : rows) {
        out << r.scenario << ',' << r.practice << ',' << r.year << ',' << r.revenue << ','
            << r.cost << ',' << r.profit << ',' << r.margin << ',' << r.utilization << ','
            << r.headcount << ',' << r.tm_backlog_hours << ',' << r.fixed_backlog_hours << '\n';
    }
}

void write_summary(std::ostream& out, const std::vector<ankurafathom::ScenarioSummary>& rows) {
    out << "scenario,incremental_npv_usd,payback_month\n";
    out << std::fixed << std::setprecision(6);
    for (const auto& r : rows) {
        out << r.scenario << ',' << r.incremental_npv << ',';
        if (r.payback_month > 0) out << r.payback_month;
        out << '\n';
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc > 1 && (std::string(argv[1]) == "lint" || std::string(argv[1]) == "check" || std::string(argv[1]) == "explain" || std::string(argv[1]) == "viz" || std::string(argv[1]) == "run" ||
                        std::string(argv[1]) == "bundle" || std::string(argv[1]) == "replay" || std::string(argv[1]) == "verify-results"))
            return ankurafathom::ir::cli(argc, argv);
        std::string input;
        std::string output;
        std::string summary;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help") {
                std::cout << "Usage:\n"
                          << "  fathom lint model.json\n"
                          << "  fathom viz model.json --out new-directory [--output ID] [--manifest run.json]\n"
                          << "  fathom explain results.parquet|results.csv|run.json|bundle --output ID --at T [--from T] [--scenario N] [--replication N] [--threads N] [--out explanation.json|html] [--format json|html]\n"
                          << "  fathom check model.json [--experiment experiment.json] [--threads N] [--seed S] [--out validation.json]\n"
                          << "  fathom run model.json [--out results.csv|parquet|arrow] [--format csv|parquet|arrow] [--experiment experiment.json] [--threads N] [--seed S] [--manifest run.json | --no-manifest] [--require-check]\n"
                          << "  fathom bundle run.json --out bundle-directory\n"
                          << "  fathom replay run.json|bundle-directory [--out results.csv|parquet|arrow] [--format csv|parquet|arrow] [--threads N]\n"
                          << "  fathom verify-results run.json [--results results.csv|parquet|arrow] [--format csv|parquet|arrow]\n"
                          << "  fathom verify-results results.parquet|arrow --embedded [--format parquet|arrow]\n"
                          << "  fathom --input practices.csv [--output yearly.csv] [--summary summary.csv]\n";
                return 0;
            }
            if (arg != "--input" && arg != "--output" && arg != "--summary")
                throw std::invalid_argument("unknown argument: " + arg);
            if (i + 1 >= argc) throw std::invalid_argument("missing value for " + arg);
            const std::string value = argv[++i];
            if (arg == "--input") input = value;
            if (arg == "--output") output = value;
            if (arg == "--summary") summary = value;
        }
        if (input.empty()) throw std::invalid_argument("--input is required");
        if ((!output.empty() && output == input) || (!summary.empty() && summary == input))
            throw std::invalid_argument("an output path cannot overwrite the input");
        if (!summary.empty() && summary == output) throw std::invalid_argument("output and summary paths must differ");
        const auto practices = ankurafathom::load_practices_csv(input);
        std::vector<ankurafathom::PracticeRun> baseline;
        for (const auto& practice : practices) baseline.push_back(ankurafathom::simulate(practice, 0));
        std::vector<ankurafathom::AnnualResult> yearly;
        std::vector<ankurafathom::ScenarioSummary> summaries;
        for (std::uint8_t flags = 0; flags < 16; ++flags) {
            std::vector<ankurafathom::PracticeRun> runs;
            for (const auto& practice : practices) runs.push_back(ankurafathom::simulate(practice, flags));
            const auto name = ankurafathom::scenario_name(flags);
            auto annual = ankurafathom::annualize(runs, name);
            yearly.insert(yearly.end(), annual.begin(), annual.end());
            summaries.push_back(ankurafathom::summarize(baseline, runs, name));
        }
        if (output.empty()) {
            write_yearly(std::cout, yearly);
        } else {
            std::ofstream file(output);
            if (!file) throw std::runtime_error("cannot open output: " + output);
            write_yearly(file, yearly);
        }
        if (!summary.empty()) {
            std::ofstream file(summary);
            if (!file) throw std::runtime_error("cannot open summary: " + summary);
            write_summary(file, summaries);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "fathom: " << error.what() << '\n';
        return 1;
    }
}
