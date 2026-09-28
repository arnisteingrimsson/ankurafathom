#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ankurafathom::abm {

template <typename Agent>
class SyncPopulation {
public:
    struct Record {
        std::uint64_t id;
        Agent value;
        bool alive;
    };
    using Snapshot = std::vector<Record>;
    using Rule = std::function<Agent(std::size_t, const Snapshot&)>;

    std::uint64_t spawn(Agent value) {
        if (stepping_) throw std::logic_error("cannot spawn during a population phase");
        if (next_id_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("agent ID exhausted");
        const auto id = next_id_;
        records_.push_back(Record{id, std::move(value), true});
        ++next_id_;
        ++active_count_;
        return id;
    }

    void despawn(std::uint64_t id) {
        if (stepping_) throw std::logic_error("cannot despawn during a population phase");
        if (id >= records_.size() || !records_[static_cast<std::size_t>(id)].alive)
            throw std::out_of_range("agent ID is not active");
        records_[static_cast<std::size_t>(id)].alive = false;
        --active_count_;
    }

    void replace(std::uint64_t id, Agent value) {
        if (stepping_) throw std::logic_error("cannot replace an agent during a population phase");
        if (id >= records_.size() || !records_[static_cast<std::size_t>(id)].alive)
            throw std::out_of_range("agent ID is not active");
        records_[static_cast<std::size_t>(id)].value = std::move(value);
    }

    void add_phase(Rule rule) {
        if (stepping_) throw std::logic_error("cannot change phases during a population step");
        if (!rule) throw std::invalid_argument("empty population phase");
        phases_.push_back(std::move(rule));
    }

    void step() {
        if (stepping_) throw std::logic_error("recursive population step");
        stepping_ = true;
        try {
            Snapshot working = records_;
            for (const auto& phase : phases_) {
                const Snapshot snapshot = working;
                Snapshot next = working;
                for (std::size_t i = 0; i < snapshot.size(); ++i) {
                    if (snapshot[i].alive) next[i].value = phase(i, snapshot);
                }
                working = std::move(next);
            }
            records_ = std::move(working);
            stepping_ = false;
        } catch (...) {
            stepping_ = false;
            throw;
        }
    }

    const Snapshot& records() const noexcept { return records_; }
    std::size_t active_count() const noexcept { return active_count_; }

private:
    Snapshot records_;
    std::vector<Rule> phases_;
    std::uint64_t next_id_ = 0;
    std::size_t active_count_ = 0;
    bool stepping_ = false;
};

} // namespace ankurafathom::abm
