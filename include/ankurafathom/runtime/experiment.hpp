#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>

namespace ankurafathom::runtime {

// Scenario IDs and replication indices are the exact values used in the
// Philox draw address. Their ranges are validated before a trajectory runs.
struct Scenario {
    std::uint32_t id;
    std::map<std::string, double> parameters;
};

struct Experiment {
    std::uint64_t seed;
    std::vector<Scenario> scenarios;
    std::uint32_t replications;
};

struct Observation {
    double time;
    std::string output_id;
    double value;
};

struct Trajectory {
    std::uint32_t scenario;
    std::uint32_t replication;
    std::vector<Observation> observations;
};

struct Statistic {
    std::uint32_t scenario;
    double time;
    std::string output_id;
    std::size_t count;
    double mean;
    double sample_variance;
};

struct ExperimentProgress { std::size_t completed,total; };
struct ExecutionOptions {
    std::size_t threads=1;
    std::stop_token stop={};
    std::function<bool(const ExperimentProgress&)> progress={};
};
class ExperimentCancelled final:public std::runtime_error {
public:
    ExperimentCancelled(std::size_t completed,std::size_t total)
        :std::runtime_error("experiment cancelled"),completed(completed),total(total) {}
    std::size_t completed,total;
};

inline void validate(const Experiment& experiment) {
    if (experiment.scenarios.empty()) throw std::invalid_argument("experiment needs scenarios");
    if (experiment.replications == 0 || experiment.replications > 65536)
        throw std::invalid_argument("replications must be in [1, 65536]");
    std::uint32_t previous = 0;
    bool first = true;
    for (const auto& scenario : experiment.scenarios) {
        if (scenario.id > 65535 || (!first && scenario.id <= previous))
            throw std::invalid_argument("scenario IDs must increase strictly and fit in 16 bits");
        for (const auto& [name, value] : scenario.parameters) {
            if (name.empty() || !std::isfinite(value))
                throw std::invalid_argument("scenario parameter must have a name and finite value");
        }
        first = false;
        previous = scenario.id;
    }
}

inline void validate_observations(const std::vector<Observation>& observations) {
    std::map<std::pair<double,std::string>,bool> seen;
    for(const auto& observation:observations) {
        // Observations may use an imported SD source clock before zero. Model
        // schedulers validate elapsed time; publication only requires finiteness.
        if(!std::isfinite(observation.time) || observation.output_id.empty() || !std::isfinite(observation.value))
            throw std::invalid_argument("trajectory contains an invalid observation");
        if(!seen.emplace(std::make_pair(observation.time,observation.output_id),true).second)
            throw std::invalid_argument("trajectory contains a duplicate time/output pair");
    }
}

// Pure value-owned callbacks and private slots separate trajectory execution
// from ordered publication. See docs/SEMANTICS.md, "Ordered experiment execution".
template<typename Run>
std::vector<Trajectory> run_experiment(const Experiment& experiment,Run&& run,const ExecutionOptions& options={}) {
    validate(experiment);
    if(options.threads==0 || options.threads>256) throw std::invalid_argument("experiment threads must be in [1, 256]");
    if(experiment.scenarios.size()>1000000/experiment.replications)
        throw std::invalid_argument("experiment exceeds 1000000 trajectories");
    const auto total=experiment.scenarios.size()*experiment.replications;
    if(options.stop.stop_requested()) throw ExperimentCancelled(0,total);
    using Callback=std::decay_t<Run>;
    static_assert(std::is_copy_constructible_v<Callback>,"experiment callback must be value-copyable");
    const Callback prototype(std::forward<Run>(run));
    std::vector<Trajectory> result;result.reserve(total);
    const auto progress=[&] {
        if(options.stop.stop_requested() || (options.progress && !options.progress({result.size(),total})) || options.stop.stop_requested())
            throw ExperimentCancelled(result.size(),total);
    };
    const auto execute=[&](std::size_t index) {
        const auto& scenario=experiment.scenarios[index/experiment.replications];
        const auto replication=static_cast<std::uint32_t>(index%experiment.replications);
        auto callback=prototype;
        auto observations=callback(scenario,replication,experiment.seed);
        validate_observations(observations);
        return Trajectory{scenario.id,replication,std::move(observations)};
    };
    progress();
    if(options.threads==1 || total==1) {
        for(std::size_t index=0;index<total;++index) {
            if(options.stop.stop_requested()) throw ExperimentCancelled(result.size(),total);
            std::optional<Trajectory> trajectory;
            try { trajectory=execute(index); }
            catch(...) {
                if(options.stop.stop_requested()) throw ExperimentCancelled(result.size(),total);
                throw;
            }
            if(options.stop.stop_requested()) throw ExperimentCancelled(result.size(),total);
            result.push_back(std::move(*trajectory));progress();
        }
        return result;
    }
    struct Slot { std::optional<Trajectory> trajectory;std::exception_ptr error;bool ready=false; };
    std::vector<Slot> slots(total);
    std::mutex mutex;std::condition_variable ready;
    // Serialize a stop notification with predicate inspection, avoiding a lost
    // wakeup when cancellation arrives between that inspection and the wait.
    std::stop_callback wake_on_stop(options.stop,[&] { std::lock_guard lock(mutex);ready.notify_all(); });
    std::atomic<std::size_t> next{0};std::atomic<bool> halt{false};
    std::vector<std::jthread> workers;
    struct Join {
        std::atomic<bool>& halt;std::vector<std::jthread>& workers;
        ~Join() { halt.store(true);workers.clear(); }
    } join{halt,workers};
    const auto count=std::min(options.threads,total);workers.reserve(count);
    for(std::size_t worker=0;worker<count;++worker) workers.emplace_back([&] {
        while(!halt.load() && !options.stop.stop_requested()) {
            const auto index=next.fetch_add(1);if(index>=total) break;
            std::optional<Trajectory> trajectory;std::exception_ptr error;
            try { trajectory=execute(index); }catch(...) { error=std::current_exception(); }
            {
                std::lock_guard lock(mutex);
                slots[index].trajectory=std::move(trajectory);slots[index].error=error;slots[index].ready=true;
            }
            ready.notify_all();
        }
        ready.notify_all();
    });
    for(std::size_t index=0;index<total;++index) {
        std::unique_lock lock(mutex);
        ready.wait(lock,[&] { return slots[index].ready || options.stop.stop_requested(); });
        if(options.stop.stop_requested()) throw ExperimentCancelled(result.size(),total);
        if(slots[index].error) std::rethrow_exception(slots[index].error);
        result.push_back(std::move(*slots[index].trajectory));
        lock.unlock();progress();
    }
    return result;
}

// Pairwise, ordered accumulation is deliberate: results are identical when
// trajectory execution is later parallelized but merged in this order.
inline std::vector<Statistic> summarize(const std::vector<Trajectory>& trajectories) {
    struct Accumulator {
        std::size_t count = 0;
        double mean = 0;
        double m2 = 0;
    };
    std::map<std::tuple<std::uint32_t, double, std::string>, Accumulator> groups;
    for (const auto& trajectory : trajectories) {
        for (const auto& observation : trajectory.observations) {
            auto& accumulator = groups[{trajectory.scenario, observation.time, observation.output_id}];
            ++accumulator.count;
            const double delta = observation.value - accumulator.mean;
            accumulator.mean += delta / static_cast<double>(accumulator.count);
            accumulator.m2 += delta * (observation.value - accumulator.mean);
            if (!std::isfinite(accumulator.mean) || !std::isfinite(accumulator.m2))
                throw std::overflow_error("ensemble statistic overflow");
        }
    }
    std::vector<Statistic> result;
    result.reserve(groups.size());
    for (const auto& [key, accumulator] : groups) {
        result.push_back(Statistic{std::get<0>(key), std::get<1>(key), std::get<2>(key),
            accumulator.count, accumulator.mean,
            accumulator.count > 1 ? accumulator.m2 / static_cast<double>(accumulator.count - 1) : 0});
    }
    return result;
}

} // namespace ankurafathom::runtime
