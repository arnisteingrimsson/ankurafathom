#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ankurafathom::sd {

enum class Integrator { euler, midpoint, rk4 };

class Model {
public:
    using State = std::vector<double>;
    using Rate = std::function<double(const State&, double)>;
    static constexpr std::size_t boundary = std::numeric_limits<std::size_t>::max();

    std::size_t add_stock(std::string name, double initial, bool nonnegative = true,
                          bool clip_outflows = false) {
        if (name.empty() || !std::isfinite(initial) || (nonnegative && initial < 0))
            throw std::invalid_argument("invalid stock name or initial value");
        if (clip_outflows && !nonnegative)
            throw std::invalid_argument("clipped stock must be nonnegative");
        for (const auto& prior : names_) {
            if (prior == name) throw std::invalid_argument("duplicate stock name");
        }
        const auto id = values_.size();
        names_.push_back(std::move(name));
        values_.push_back(initial);
        nonnegative_.push_back(nonnegative);
        clip_outflows_.push_back(clip_outflows);
        outflow_order_.emplace_back();
        return id;
    }

    std::size_t add_flow(std::size_t source, std::size_t destination, Rate rate,
                        bool nonnegative = true, bool clip_negative = false) {
        if ((source != boundary && source >= values_.size()) ||
            (destination != boundary && destination >= values_.size()) ||
            (source == boundary && destination == boundary) || !rate)
            throw std::invalid_argument("invalid flow endpoints or rate");
        if (clip_negative && !nonnegative)
            throw std::invalid_argument("clipped flow must be nonnegative");
        flows_.push_back(Flow{source, destination, std::move(rate), nonnegative, clip_negative});
        return flows_.size()-1;
    }

    void set_outflow_order(std::size_t stock, std::vector<std::size_t> order) {
        if (stock >= values_.size() || !clip_outflows_[stock])
            throw std::invalid_argument("outflow order requires a clipped stock");
        auto expected = outgoing(stock);
        auto sorted = order;
        std::sort(sorted.begin(), sorted.end());
        if (sorted != expected)
            throw std::invalid_argument("outflow order must list every outgoing flow exactly once");
        outflow_order_[stock] = std::move(order);
    }

    void validate_clipping() const { (void)clipping_order(); }

    const State& state() const noexcept { return values_; }

    void add_to_stock(std::size_t id, double amount) {
        if (id >= values_.size() || !std::isfinite(amount) ||
            !std::isfinite(values_[id] + amount) ||
            (nonnegative_[id] && values_[id] + amount < 0))
            throw std::invalid_argument("invalid stock pulse");
        values_[id] += amount;
    }

    State derivative(const State& state, double time) const {
        if (has_clipping())
            throw std::logic_error("clipped flows require a complete Euler step with dt");
        if (state.size() != values_.size() || !std::isfinite(time))
            throw std::invalid_argument("invalid derivative state or time");
        State result(state.size(), 0);
        for (const auto& flow : flows_) {
            const double value = flow.rate(state, time);
            if (!std::isfinite(value) || (flow.nonnegative && value < 0))
                throw std::domain_error("flow rate must be finite and satisfy its sign policy");
            if (flow.source != boundary) result[flow.source] -= value;
            if (flow.destination != boundary) result[flow.destination] += value;
        }
        return result;
    }

    void step(double time, double dt, Integrator method = Integrator::euler) {
        if (!std::isfinite(time) || !std::isfinite(dt) || dt <= 0 || !std::isfinite(time + dt))
            throw std::invalid_argument("invalid SD time step");
        if (has_clipping()) {
            if (method != Integrator::euler)
                throw std::invalid_argument("clipping requires Euler integration");
            values_ = clipped_euler_candidate(time, dt);
            return;
        }
        const State k1 = derivative(values_, time);
        State next = values_;
        if (method == Integrator::euler) {
            add_scaled(next, k1, dt);
        } else if (method == Integrator::midpoint) {
            State midpoint = values_;
            add_scaled(midpoint, k1, dt / 2);
            const State k2 = derivative(midpoint, time + dt / 2);
            add_scaled(next, k2, dt);
        } else if (method == Integrator::rk4) {
            State trial = values_;
            add_scaled(trial, k1, dt / 2);
            const State k2 = derivative(trial, time + dt / 2);
            trial = values_;
            add_scaled(trial, k2, dt / 2);
            const State k3 = derivative(trial, time + dt / 2);
            trial = values_;
            add_scaled(trial, k3, dt);
            const State k4 = derivative(trial, time + dt);
            for (std::size_t i = 0; i < next.size(); ++i)
                next[i] += dt * (k1[i] + 2 * k2[i] + 2 * k3[i] + k4[i]) / 6;
        } else {
            throw std::invalid_argument("unknown integrator");
        }
        for (std::size_t i = 0; i < next.size(); ++i) {
            if (!std::isfinite(next[i])) throw std::domain_error("non-finite stock after integration");
            if (nonnegative_[i] && next[i] < 0)
                throw std::domain_error("negative stock after integration: " + names_[i]);
        }
        values_ = std::move(next);
    }

private:
    struct Flow {
        std::size_t source;
        std::size_t destination;
        Rate rate;
        bool nonnegative;
        bool clip_negative;
    };

    bool has_clipping() const {
        return std::any_of(clip_outflows_.begin(), clip_outflows_.end(), [](bool x) { return x; }) ||
            std::any_of(flows_.begin(), flows_.end(), [](const Flow& f) { return f.clip_negative; });
    }

    std::vector<std::size_t> outgoing(std::size_t stock) const {
        std::vector<std::size_t> result;
        for (std::size_t i=0; i<flows_.size(); ++i)
            if (flows_[i].source == stock) result.push_back(i);
        return result;
    }

    std::vector<std::size_t> clipping_order() const {
        std::vector<std::size_t> indegree(values_.size(), 0), order;
        std::size_t count=0;
        for (std::size_t i=0; i<values_.size(); ++i) if (clip_outflows_[i]) {
            ++count;
            auto sorted=outflow_order_[i];
            std::sort(sorted.begin(),sorted.end());
            if (sorted != outgoing(i))
                throw std::invalid_argument("outflow order must list every outgoing flow exactly once");
        }
        for (const auto& f : flows_) {
            const bool source = f.source != boundary && clip_outflows_[f.source];
            const bool destination = f.destination != boundary && clip_outflows_[f.destination];
            if ((source || destination) && !f.nonnegative)
                throw std::invalid_argument("clipped stocks require nonnegative incident flows");
            if (source && destination) ++indegree[f.destination];
        }
        for (std::size_t i=0; i<values_.size(); ++i)
            if (clip_outflows_[i] && indegree[i]==0) order.push_back(i);
        for (std::size_t k=0; k<order.size(); ++k)
            for (const auto& f : flows_)
                if (f.source==order[k] && f.destination!=boundary && clip_outflows_[f.destination])
                    if (--indegree[f.destination]==0) order.push_back(f.destination);
        if (order.size()!=count)
            throw std::invalid_argument("cyclic clipped-stock flow dependencies are unsupported");
        return order;
    }

    static double finite(double value) {
        if (!std::isfinite(value)) throw std::overflow_error("clipped Euler arithmetic overflow");
        return value;
    }

    State clipped_euler_candidate(double time, double dt) const {
        const auto order=clipping_order();
        std::vector<double> amounts;
        amounts.reserve(flows_.size());
        for (const auto& f : flows_) {
            double rate=finite(f.rate(values_,time));
            if (f.clip_negative) rate=std::max(0.,rate);
            if (f.nonnegative && rate<0) throw std::domain_error("negative flow rate");
            amounts.push_back(finite(dt*rate));
        }
        State next=values_;
        // All requested rates read old state. Already constrained upstream
        // amounts then fund downstream stocks in this same Euler interval.
        for (const auto stock : order) {
            double available=values_[stock];
            for (std::size_t i=0; i<flows_.size(); ++i)
                if (flows_[i].destination==stock) available=finite(available+amounts[i]);
            for (const auto flow : outflow_order_[stock]) {
                amounts[flow]=std::min(amounts[flow],available);
                available=finite(available-amounts[flow]);
            }
            next[stock]=available;
        }
        // A shared flow has one amount at both endpoints; never clamp a stock
        // after integration, which would create material at the receiving end.
        for (std::size_t i=0; i<flows_.size(); ++i) {
            const auto& f=flows_[i];
            if (f.source!=boundary && !clip_outflows_[f.source])
                next[f.source]=finite(next[f.source]-amounts[i]);
            if (f.destination!=boundary && !clip_outflows_[f.destination])
                next[f.destination]=finite(next[f.destination]+amounts[i]);
        }
        for (std::size_t i=0; i<next.size(); ++i)
            if (!std::isfinite(next[i]) || (nonnegative_[i] && next[i]<0))
                throw std::domain_error("stock left valid domain during clipped Euler step");
        return next;
    }

    static void add_scaled(State& destination, const State& increment, double scale) {
        for (std::size_t i = 0; i < destination.size(); ++i) destination[i] += increment[i] * scale;
    }

    std::vector<std::string> names_;
    State values_;
    std::vector<bool> nonnegative_;
    std::vector<bool> clip_outflows_;
    std::vector<std::vector<std::size_t>> outflow_order_;
    std::vector<Flow> flows_;
};

} // namespace ankurafathom::sd
