#pragma once

#include "ankurafathom/hybrid/transactional_agent_pool.hpp"

#include <map>
#include <optional>
#include <set>

namespace ankurafathom::hybrid {

// Non-preemptive, fixed-duration delivery backed by individually owned capacity.
// Calendar, engagement records, statistics and staffing commit as one state.
template <typename Agent>
class AgentPoolProcess {
public:
    using Pool = TransactionalAgentPool<Agent>;
    struct Engagement {
        std::uint64_t request_id;
        std::size_t units;
        double duration;
    };
    struct Change {
        typename Pool::Change workforce;
        std::vector<Engagement> arrivals;
    };
    struct Record {
        double arrival;
        double duration;
        std::size_t units;
        std::optional<double> started;
        std::optional<double> completed;
    };
    struct Statistics {
        std::size_t accepted = 0;
        std::size_t started = 0;
        std::size_t completed = 0;
        double wait_total = 0;
        double cycle_total = 0;
    };
    struct TimeStatistics {
        double elapsed = 0;
        double capacity_time = 0;
        double allocated_time = 0;
        double available_time = 0;
        double queue_time = 0;
        double service_time = 0;
        double population_time = 0;

        double utilization() const noexcept {
            return capacity_time > 0 ? allocated_time / capacity_time : 0;
        }
        double mean_queue() const noexcept { return elapsed > 0 ? queue_time / elapsed : 0; }
        double mean_in_service() const noexcept { return elapsed > 0 ? service_time / elapsed : 0; }
        double mean_headcount() const noexcept { return elapsed > 0 ? population_time / elapsed : 0; }
    };
    struct Result {
        typename Pool::Result staffing;
        std::vector<std::uint64_t> completed;
    };

    explicit AgentPoolProcess(Pool pool) : state_(std::make_unique<State>(std::move(pool))) {
        if (system().pool().waiting() != 0 || system().pool().allocated() != 0)
            throw std::invalid_argument("delivery requires an initially idle staffing pool");
    }

    AgentPoolProcess(const AgentPoolProcess& source)
        : state_(std::make_unique<State>(*source.state_)) {}
    AgentPoolProcess& operator=(const AgentPoolProcess& source) {
        if (this == &source) return *this;
        auto candidate = std::make_unique<State>(*source.state_);
        state_ = std::move(candidate);
        return *this;
    }
    AgentPoolProcess(AgentPoolProcess&&) noexcept = default;
    AgentPoolProcess& operator=(AgentPoolProcess&&) noexcept = default;

    Result apply(const Change& change) {
        const double time = change.workforce.time;
        if (!std::isfinite(time) || time < system().now() || time > next_completion())
            throw std::invalid_argument("delivery time is invalid or skips a pending completion");
        if (!change.workforce.requests.empty() || !change.workforce.releases.empty())
            throw std::invalid_argument("delivery owns all staffing requests and releases");
        auto candidate = std::make_unique<State>(*state_);
        // The old state holds on [last event, this event); event actions have zero duration.
        candidate->time_statistics = integrate(*state_, time);
        auto transaction = change.workforce;
        Result result;
        while (!candidate->calendar.empty() && candidate->calendar.begin()->first == time) {
            const auto id = candidate->calendar.begin()->second;
            auto& record = candidate->records.at(id);
            transaction.releases.push_back({id});
            record.completed = time;
            ++candidate->statistics.completed;
            candidate->statistics.cycle_total += time - record.arrival;
            result.completed.push_back(id);
            candidate->calendar.erase(candidate->calendar.begin());
        }
        for (const auto& arrival : change.arrivals) {
            if (!std::isfinite(arrival.duration) || arrival.duration <= 0 || arrival.units == 0)
                throw std::invalid_argument("engagement needs positive units and finite positive duration");
            if (!candidate->records.emplace(arrival.request_id, Record{
                    time, arrival.duration, arrival.units, std::nullopt, std::nullopt}).second)
                throw std::invalid_argument("engagement ID cannot be reused");
            transaction.requests.push_back({arrival.request_id, arrival.units});
            ++candidate->statistics.accepted;
        }
        result.staffing = candidate->pool.apply(transaction);
        for (const auto& grant : result.staffing.grants) {
            auto& record = candidate->records.at(grant.request_id);
            const double finish = time + record.duration;
            if (!std::isfinite(finish) || finish <= time)
                throw std::overflow_error("completion must be finite and strictly after delivery start");
            record.started = time;
            candidate->calendar.emplace(finish, grant.request_id);
            ++candidate->statistics.started;
            candidate->statistics.wait_total += time - record.arrival;
        }
        const auto& statistics = candidate->statistics;
        if (!std::isfinite(statistics.wait_total) || !std::isfinite(statistics.cycle_total))
            throw std::overflow_error("delivery statistics overflow");
        if (statistics.accepted != candidate->records.size() ||
            candidate->pool.pool().waiting() != statistics.accepted - statistics.started ||
            candidate->pool.pool().allocated() != statistics.started - statistics.completed ||
            candidate->calendar.size() != statistics.started - statistics.completed)
            throw std::logic_error("delivery conservation invariant failed");
        state_ = std::move(candidate);
        return result;
    }

    double next_completion() const noexcept {
        return state_->calendar.empty() ? std::numeric_limits<double>::infinity()
                                        : state_->calendar.begin()->first;
    }
    const Pool& system() const noexcept { return state_->pool; }
    const Statistics& statistics() const noexcept { return state_->statistics; }
    const std::map<std::uint64_t, Record>& records() const noexcept { return state_->records; }
    TimeStatistics time_statistics(double time) const {
        if (!std::isfinite(time) || time < system().now() || time > next_completion())
            throw std::invalid_argument("statistics time precedes committed state or skips a completion");
        // Read-only projection: sampling frequency must not change the accumulated sums.
        return integrate(*state_, time);
    }

private:
    struct State {
        explicit State(Pool source) : pool(std::move(source)), origin(pool.now()) {}
        Pool pool;
        double origin;
        std::map<std::uint64_t, Record> records;
        std::set<std::pair<double, std::uint64_t>> calendar;
        Statistics statistics;
        TimeStatistics time_statistics;
    };
    static TimeStatistics integrate(const State& state, double time) {
        auto result = state.time_statistics;
        const double elapsed = time - state.pool.now();
        const auto& pool = state.pool.pool();
        result.elapsed = time - state.origin;
        result.capacity_time += static_cast<double>(pool.capacity()) * elapsed;
        result.allocated_time += static_cast<double>(pool.allocated_units()) * elapsed;
        result.available_time += static_cast<double>(pool.available()) * elapsed;
        result.queue_time += static_cast<double>(pool.waiting()) * elapsed;
        result.service_time += static_cast<double>(pool.allocated()) * elapsed;
        result.population_time += static_cast<double>(state.pool.population().active_count()) * elapsed;
        for (double value : {result.elapsed, result.capacity_time, result.allocated_time,
                             result.available_time, result.queue_time, result.service_time,
                             result.population_time})
            if (!std::isfinite(value)) throw std::overflow_error("delivery time statistics overflow");
        return result;
    }
    std::unique_ptr<State> state_;
};

} // namespace ankurafathom::hybrid
