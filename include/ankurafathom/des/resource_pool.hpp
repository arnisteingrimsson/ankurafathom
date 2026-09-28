#pragma once

#include "ankurafathom/devs/simulator.hpp"
#include "ankurafathom/des/queue_discipline.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace ankurafathom::des {

struct Seize {
    std::uint64_t request_id;
    std::size_t units;
    std::int32_t priority = 0;
    bool preempt = false;
};

struct Release {
    std::uint64_t request_id;
};

struct Grant {
    std::uint64_t request_id;
    std::size_t units;
};

struct SetCapacity {
    std::size_t units;
};

struct CapacitySample {};

struct AgentAssigned {
    std::uint64_t request_id;
    std::uint64_t agent_id;
    std::size_t units;
};

struct AgentUnassigned {
    std::uint64_t request_id;
    std::uint64_t agent_id;
    std::size_t units;
};

using ResourceMessage = std::variant<Seize, Release, Grant, SetCapacity, CapacitySample,
                                     AgentAssigned, AgentUnassigned>;

class ResourcePool final : public devs::Atomic<ResourceMessage> {
public:
    static constexpr std::uint32_t input_port = 0;
    static constexpr std::uint32_t output_port = 1;
    static constexpr std::uint32_t capacity_port = 2;

    explicit ResourcePool(std::size_t capacity) : ResourcePool(capacity, capacity) {}

    ResourcePool(std::size_t capacity, std::size_t max_request_units,
                 QueueDiscipline discipline = QueueDiscipline::fifo)
        : capacity_(capacity), available_(capacity), max_request_units_(max_request_units), discipline_(discipline) {
        if (max_request_units == 0)
            throw std::invalid_argument("resource request limit must be positive");
        if (!valid_queue_discipline(discipline))
            throw std::invalid_argument("unsupported resource queue discipline");
    }

    std::unique_ptr<devs::Atomic<ResourceMessage>> clone() const override {
        return std::make_unique<ResourcePool>(*this);
    }

    double time_advance() const override {
        return pending_.empty() ? std::numeric_limits<double>::infinity() : 0;
    }

    std::vector<devs::PortValue<ResourceMessage>> output() const override {
        std::vector<devs::PortValue<ResourceMessage>> result;
        result.reserve(pending_.size());
        for (const auto& grant : pending_)
            result.push_back({output_port, ResourceMessage{grant}});
        return result;
    }

    void internal_transition() override {
        if (pending_.empty()) throw std::logic_error("resource pool has no internal event");
        pending_.clear();
    }

    void external_transition(double elapsed,
                             const std::vector<devs::Input<ResourceMessage>>& bag) override {
        if (!std::isfinite(elapsed) || elapsed < 0)
            throw std::invalid_argument("invalid resource pool elapsed time");
        accept(bag, false);
    }

    void confluent_transition(const std::vector<devs::Input<ResourceMessage>>& bag) override {
        if (pending_.empty()) throw std::logic_error("resource pool is not imminent");
        accept(bag, true);
    }

    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t max_request_units() const noexcept { return max_request_units_; }
    std::size_t available() const noexcept { return available_; }
    std::size_t waiting() const noexcept { return waiting_.size(); }
    std::size_t allocated() const noexcept { return allocations_.size(); }
    std::size_t allocated_units() const noexcept {
        std::size_t total = 0;
        for (const auto& [_, units] : allocations_) total += units;
        return total;
    }

private:
    void accept(const std::vector<devs::Input<ResourceMessage>>& bag, bool clear_pending) {
        auto capacity = capacity_;
        auto available = available_;
        auto allocations = allocations_;
        auto waiting = waiting_;
        auto seen = seen_ids_;
        auto pending = pending_;
        if (clear_pending) pending.clear();

        const SetCapacity* capacity_update = nullptr;
        for (const auto& input : bag) {
            if (input.port == capacity_port && std::holds_alternative<SetCapacity>(input.value)) {
                if (capacity_update)
                    throw std::invalid_argument("multiple resource capacity updates in one bag");
                capacity_update = &std::get<SetCapacity>(input.value);
            } else if (input.port != input_port ||
                       (!std::holds_alternative<Seize>(input.value) &&
                        !std::holds_alternative<Release>(input.value))) {
                throw std::invalid_argument("resource pool received an invalid input or port");
            }
        }
        for (const auto& input : bag) {
            if (const auto* release = std::get_if<Release>(&input.value)) {
                const auto allocation = allocations.find(release->request_id);
                if (allocation == allocations.end())
                    throw std::invalid_argument("release references an inactive allocation");
                available += allocation->second;
                allocations.erase(allocation);
            }
        }

        if (capacity_update) {
            const auto allocated = capacity - available;
            if (capacity_update->units < allocated)
                throw std::invalid_argument("resource capacity cannot revoke active allocations");
            capacity = capacity_update->units;
            available = capacity - allocated;
        }
        std::vector<Seize> incoming;
        for (const auto& input : bag) {
            if (const auto* request = std::get_if<Seize>(&input.value)) {
                if (request->preempt || request->units == 0 || request->units > max_request_units_ ||
                    !seen.insert(request->request_id).second)
                    throw std::invalid_argument("invalid, preemptive, or duplicate resource request");
                incoming.push_back(*request);
            }
        }
        if (discipline_ == QueueDiscipline::priority)
            std::sort(incoming.begin(), incoming.end(), [](const Seize& a, const Seize& b) {
                return a.priority != b.priority ? a.priority < b.priority : a.request_id < b.request_id;
            });
        waiting.insert(waiting.end(), incoming.begin(), incoming.end());
        if (discipline_ == QueueDiscipline::priority)
            std::stable_sort(waiting.begin(), waiting.end(), [](const Seize& a, const Seize& b) {
                return a.priority < b.priority;
            });
        grant_waiters(available, allocations, waiting, pending);

        capacity_ = capacity;
        available_ = available;
        allocations_ = std::move(allocations);
        waiting_ = std::move(waiting);
        seen_ids_ = std::move(seen);
        pending_ = std::move(pending);
    }

    void grant_waiters(std::size_t& available, std::map<std::uint64_t, std::size_t>& allocations,
                       std::deque<Seize>& waiting, std::vector<Grant>& pending) const {
        while (!waiting.empty()) {
            const Seize request = discipline_ == QueueDiscipline::lifo ? waiting.back() : waiting.front();
            if (request.units > available) break;
            if (discipline_ == QueueDiscipline::lifo) waiting.pop_back();
            else waiting.pop_front();
            available -= request.units;
            allocations.emplace(request.request_id, request.units);
            pending.push_back({request.request_id, request.units});
        }
    }

    std::size_t capacity_;
    std::size_t available_;
    std::size_t max_request_units_;
    QueueDiscipline discipline_;
    std::map<std::uint64_t, std::size_t> allocations_;
    std::deque<Seize> waiting_;
    std::set<std::uint64_t> seen_ids_;
    std::vector<Grant> pending_;
};

} // namespace ankurafathom::des
