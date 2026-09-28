#include "ankurafathom/sd/model.hpp"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using ankurafathom::sd::Model;
using ankurafathom::sd::Integrator;
using Json = nlohmann::json;
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

Model model_for(const Json& spec) {
    Model model;
    const auto name = spec.at("name").get<std::string>();
    const auto& p = spec.at("parameters");
    const auto initial = spec.at("initial").get<std::vector<double>>();
    const auto names = spec.at("stocks").get<std::vector<std::string>>();
    require(initial.size() == names.size(), "invalid reference stock dimensions");
    for (std::size_t i = 0; i < initial.size(); ++i) model.add_stock(names[i], initial[i], name != "oscillator");
    if (name == "sir") {
        const double beta = p.at("beta"), gamma = p.at("gamma");
        model.add_flow(0, 1, [beta](const auto& s, double) { return beta*s[0]*s[1]; });
        model.add_flow(1, 2, [gamma](const auto& s, double) { return gamma*s[1]; });
    } else if (name == "lotka_volterra") {
        const double alpha = p.at("alpha"), beta = p.at("beta"), delta = p.at("delta"), gamma = p.at("gamma");
        model.add_flow(Model::boundary, 0, [alpha](const auto& s, double) { return alpha*s[0]; });
        model.add_flow(0, Model::boundary, [beta](const auto& s, double) { return beta*s[0]*s[1]; });
        model.add_flow(Model::boundary, 1, [delta](const auto& s, double) { return delta*s[0]*s[1]; });
        model.add_flow(1, Model::boundary, [gamma](const auto& s, double) { return gamma*s[1]; });
    } else if (name == "bass") {
        const double innovation = p.at("p"), imitation = p.at("q");
        model.add_flow(Model::boundary, 0, [innovation, imitation](const auto& s, double) {
            return (innovation+imitation*s[0])*(1-s[0]);
        });
    } else if (name == "logistic") {
        const double r = p.at("r"), capacity = p.at("capacity");
        model.add_flow(Model::boundary, 0, [r, capacity](const auto& s, double) { return r*s[0]*(1-s[0]/capacity); });
    } else if (name == "oscillator") {
        const double damping = p.at("damping"), omega = p.at("omega");
        // Signed stocks use nonnegative inflow/outflow pairs. Their net RHS is
        // smooth, including at velocity/acceleration sign changes.
        const auto acceleration = [damping, omega](const auto& s) { return -2*damping*s[1]-omega*omega*s[0]; };
        model.add_flow(Model::boundary, 0, [](const auto& s, double) { return std::max(s[1], 0.0); });
        model.add_flow(0, Model::boundary, [](const auto& s, double) { return std::max(-s[1], 0.0); });
        model.add_flow(Model::boundary, 1, [acceleration](const auto& s, double) { return std::max(acceleration(s), 0.0); });
        model.add_flow(1, Model::boundary, [acceleration](const auto& s, double) { return std::max(-acceleration(s), 0.0); });
    } else if (name == "time_growth") {
        model.add_flow(Model::boundary, 0, [](const auto& s, double time) { return time*s[0]; });
    } else throw std::runtime_error("unknown reference model: " + name);
    return model;
}

struct Errors { double maximum = 0, rms = 0, invariant_drift = 0; };
Errors trajectory(const Json& spec, Integrator method, double dt) {
    auto model = model_for(spec);
    const auto name = spec.at("name").get<std::string>();
    const double horizon = spec.at("horizon"), sample_dt = spec.at("sample_dt");
    const auto times = spec.at("times").get<std::vector<double>>();
    const auto reference = spec.at("values").get<std::vector<std::vector<double>>>();
    const auto scales = spec.at("scales").get<std::vector<double>>();
    const auto steps = static_cast<std::size_t>(std::llround(horizon/dt));
    const auto stride = static_cast<std::size_t>(std::llround(sample_dt/dt));
    require(stride > 0 && steps*dt == horizon && stride*dt == sample_dt && times.size() == reference.size() &&
            times.size() == steps/stride+1 && scales.size() == model.state().size(), name + ": invalid reference grid");
    for (const auto scale : scales) require(std::isfinite(scale) && scale > 0, "invalid reference scale");
    auto invariant = [&](const Model::State& s) {
        if (name == "sir") return s[0]+s[1]+s[2];
        if (name == "lotka_volterra") {
            const auto& p = spec.at("parameters");
            return p.at("delta").get<double>()*s[0]-p.at("gamma").get<double>()*std::log(s[0])+
                   p.at("beta").get<double>()*s[1]-p.at("alpha").get<double>()*std::log(s[1]);
        }
        return 0.0;
    };
    const double initial_invariant = invariant(model.state());
    Errors error;
    std::size_t compared = 0;
    for (std::size_t step = 0; step <= steps; ++step) {
        const auto& state = model.state();
        for (const auto value : state) {
            require(std::isfinite(value), name + ": nonfinite state");
            if (name != "oscillator") require(value >= 0, name + ": negative stock");
        }
        if (name == "lotka_volterra") require(state[0] > 0 && state[1] > 0, "nonpositive predator/prey population");
        const double drift = std::abs(invariant(state)-initial_invariant);
        require(std::isfinite(drift), name + ": nonfinite invariant");
        error.invariant_drift = std::max(error.invariant_drift, drift);
        if (name == "sir") require(drift <= 5e-13, "SIR population is not conserved");
        if (name == "bass" || name == "logistic") require(state[0] <= 1+5e-14, name + ": bounded growth exceeded capacity");
        if (step % stride == 0) {
            const auto sample = step/stride;
            require(times[sample] == step*dt && reference[sample].size() == state.size(), "reference sample mismatch");
            for (std::size_t stock = 0; stock < state.size(); ++stock) {
                require(std::isfinite(reference[sample][stock]), "nonfinite oracle value");
                const double gap = std::abs(state[stock]-reference[sample][stock])/scales[stock];
                error.maximum = std::max(error.maximum, gap);
                error.rms += gap*gap;
                ++compared;
            }
            if (name == "oscillator" && method == Integrator::euler) {
                const auto& p = spec.at("parameters");
                const double damping = p.at("damping"), omega = p.at("omega");
                const double w = std::sqrt(omega*omega-damping*damping);
                const auto z = std::pow(std::complex<double>(1-damping*dt, w*dt), static_cast<int>(step));
                require(std::abs(state[0]-(z.real()+damping/w*z.imag())) < 2e-12 &&
                        std::abs(state[1]+omega*omega/w*z.imag()) < 2e-12,
                        "oscillator Euler trajectory disagrees with its exact discrete solution");
            }
        }
        if (step == steps) break;
        const auto prior = state;
        model.step(step*dt, dt, method);
        if (name == "bass" || name == "logistic")
            require(model.state()[0] >= prior[0], name + ": adoption/population decreased");
        if (name == "sir")
            require(model.state()[0] <= prior[0] && model.state()[2] >= prior[2], "SIR transfer direction changed");
    }
    error.rms = std::sqrt(error.rms/static_cast<double>(compared));
    return error;
}

void intermediate_failure_does_not_commit() {
    for (const auto [method, stages] : std::array<std::pair<Integrator, int>, 3>{{
             {Integrator::euler, 1}, {Integrator::midpoint, 2}, {Integrator::rk4, 4}}}) {
        for (int failure_stage = 1; failure_stage <= stages; ++failure_stage) {
            Model model;
            model.add_stock("stock", 1);
            int calls = 0;
            model.add_flow(Model::boundary, 0, [&](const auto& state, double) {
                if (++calls == failure_stage) throw std::domain_error("injected stage failure");
                return state[0];
            });
            bool rejected = false;
            try { model.step(0, 0.25, method); } catch (const std::domain_error&) { rejected = true; }
            require(rejected && model.state() == Model::State{1}, "intermediate stage failure committed partial stock state");
        }
    }
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: sd_nonlinear_tests reference.json convergence.csv");
        std::ifstream input(argv[1]);
        require(static_cast<bool>(input), "cannot read SD reference data");
        const auto oracle = Json::parse(input);
        require(oracle.at("schema_version") == 1 && oracle.at("models").size() == 6, "unexpected oracle schema/model count");
        std::ofstream output(argv[2]);
        require(static_cast<bool>(output), "cannot write convergence report");
        output << std::setprecision(17);
        output << "model,integrator,dt,steps,max_scaled_error,rms_scaled_error,observed_order,invariant_drift\n";
        const std::array<Integrator, 3> methods{Integrator::euler, Integrator::midpoint, Integrator::rk4};
        const std::array<const char*, 3> names{"euler", "midpoint", "rk4"};
        const std::array<double, 3> lower_order{0.85, 1.8, 3.6}, upper_order{1.15, 2.2, 4.4};
        const std::array<double, 3> finest_error_gate{0.05, 0.003, 5e-6};
        std::set<std::string> seen;
        for (const auto& spec : oracle.at("models")) {
            const auto name = spec.at("name").get<std::string>();
            require(seen.insert(name).second, "duplicate reference model");
            double oracle_gap = std::numeric_limits<double>::epsilon();
            for (const auto& gap : spec.at("checks")) {
                const double value = gap.get<double>();
                require(std::isfinite(value) && value >= 0 && value <= 2e-11, "invalid oracle self-check");
                oracle_gap = std::max(oracle_gap, value);
            }
            const auto scales = spec.at("scales").get<std::vector<double>>();
            require(!scales.empty(), "missing error scales");
            const double noise_floor = oracle_gap / *std::min_element(scales.begin(), scales.end());
            for (std::size_t method = 0; method < methods.size(); ++method) {
                double prior_error = 0;
                for (int refinement = 0; refinement < 4; ++refinement) {
                    const double dt = spec.at("base_dt").get<double>()/std::pow(2, refinement);
                    const auto error = trajectory(spec, methods[method], dt);
                    require(error.maximum > 0, name + ": reference error is zero; convergence order is not measurable");
                    const double order = refinement ? std::log2(prior_error/error.maximum) : 0;
                    output << name << ',' << names[method] << ',' << dt << ','
                           << std::llround(spec.at("horizon").get<double>()/dt) << ','
                           << error.maximum << ',' << error.rms << ',';
                    if (refinement) output << order;
                    output << ',';
                    if (name == "sir" || name == "lotka_volterra") output << error.invariant_drift;
                    output << '\n';
                    if (refinement) require(error.maximum < prior_error, name + " " + names[method] + ": error did not decrease");
                    if (refinement >= 2)
                        require(order >= lower_order[method] && order <= upper_order[method],
                                name + " " + names[method] + ": unexpected convergence order " + std::to_string(order));
                    if (refinement == 3)
                        require(error.maximum <= finest_error_gate[method] && error.maximum > 100*noise_floor,
                                name + " " + names[method] + ": finest error exceeded gate or reached oracle noise floor");
                    prior_error = error.maximum;
                }
            }
        }
        require(seen == std::set<std::string>{"sir", "lotka_volterra", "bass", "logistic", "oscillator", "time_growth"},
                "reference case coverage changed");
        intermediate_failure_does_not_commit();
        output.close();
        require(static_cast<bool>(output), "failed writing SD convergence report");
        std::cout << "Six SD models, three integrators, four resolutions: 72 trajectories and stage rollback checks pass\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
